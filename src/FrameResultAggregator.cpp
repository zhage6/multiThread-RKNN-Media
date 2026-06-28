#include "FrameResultAggregator.h"

FrameResultAggregator::FrameResultAggregator()
{
}

FrameResultAggregator::~FrameResultAggregator()
{
    Stop();
}

void FrameResultAggregator::SetRequiredModels(std::vector<ModelId> models)
{
    std::lock_guard<std::mutex> lock(mtx_);
    required_models_ = std::move(models);
}

void FrameResultAggregator::SetTimeout(std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> lock(mtx_);
    timeout_ = timeout;
}

void FrameResultAggregator::SetOutputCallback(AggregatedFrameCallback cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    on_frame_ready_ = std::move(cb);
}

bool FrameResultAggregator::Start()
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (running_) {
        return true;
    }

    running_ = true;
    worker_ = std::thread(&FrameResultAggregator::WorkerLoop, this);
    return true;
}

void FrameResultAggregator::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& output : queue_) 
    {
        ReleaseOutput(output);
    }
    queue_.clear();

    for (auto& item : pending_) 
    {
        ReleaseAggregate(item.second);
    }
    pending_.clear();
    expected_publish_frame_.clear();
}
void FrameResultAggregator::ReleaseFrame(FrameContext& frame)
{
    if (frame.src_buffer) 
    {
        mpp_buffer_put(frame.src_buffer);
        frame.src_buffer = nullptr;
        frame.src_fd = -1;
    }
}

void FrameResultAggregator::ReleaseOutput(ModelOutput& output)
{
    ReleaseFrame(output.frame);
}

void FrameResultAggregator::ReleaseAggregate(FrameAggregate& agg)
{
    ReleaseFrame(agg.frame);
}

bool FrameResultAggregator::IsFinishedLocked(const FrameKey& key) const
{
    return finished_frames_.find(key) != finished_frames_.end();
}

void FrameResultAggregator::RememberFinishedLocked(const FrameKey& key)
{
    if (finished_frames_.insert(key).second) {
        finished_order_.push_back(key);
    }

    while (finished_order_.size() > max_finished_history_) {
        finished_frames_.erase(finished_order_.front());
        finished_order_.pop_front();
    }
}

bool FrameResultAggregator::Submit(ModelOutput output)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (!running_) 
    {
        return false;
    }

    FrameKey key;
    key.channel_id = output.frame.channel_id;
    key.frame_id = output.frame.frame_id;

    if (IsFinishedLocked(key)) 
    {
        ReleaseOutput(output);
        return true;
    }

    queue_.push_back(std::move(output));
    cv_.notify_one();
    return true;
}

void FrameResultAggregator::SkipFrame(int channel_id, uint64_t frame_id)
{
    std::lock_guard<std::mutex> lock(mtx_);

    FrameKey key;
    key.channel_id = channel_id;
    key.frame_id = frame_id;

    if (IsFinishedLocked(key)) {
        return;
    }

    auto pending = pending_.find(key);
    if (pending != pending_.end()) {
        ReleaseAggregate(pending->second);
        pending_.erase(pending);
    }

    RememberFinishedLocked(key);

    auto expected = expected_publish_frame_.find(channel_id);
    if (expected == expected_publish_frame_.end()) {
        expected_publish_frame_[channel_id] = frame_id + 1;
    } else if (frame_id >= expected->second) {
        expected->second = frame_id + 1;
    }

    PublishAvailableLocked();
}

void FrameResultAggregator::WorkerLoop()
{
    while (true) 
    {
        ModelOutput output;

        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() 
            {
                return !running_ || !queue_.empty();
            });

            if (!running_ && queue_.empty()) 
            {
                break;
            }

            if (!queue_.empty()) 
            {
                output = std::move(queue_.front());
                queue_.pop_front();
            } 
            else 
            {
                PublishTimeoutLocked();
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mtx_);
            AddResult(std::move(output));
            PublishReadyLocked();
            PublishTimeoutLocked();
        }
    }
}

void FrameResultAggregator::AddResult(ModelOutput output)
{
    FrameKey key;
    key.channel_id = output.frame.channel_id;
    key.frame_id = output.frame.frame_id;

    if (IsFinishedLocked(key)) 
    {
        ReleaseOutput(output);
        return;
    }

    auto expected = expected_publish_frame_.find(key.channel_id);
    if (expected == expected_publish_frame_.end()) {
        expected_publish_frame_[key.channel_id] = key.frame_id;
    } else if (key.frame_id < expected->second) {
        ReleaseOutput(output);
        RememberFinishedLocked(key);
        return;
    }

    auto& agg = pending_[key];
    bool first_result = agg.results.empty();
    if (first_result) 
    {
        agg.frame = output.frame;
        agg.first_seen = std::chrono::steady_clock::now();
    }


    // 同一个模型同一帧重复回来时，用最新结果覆盖旧结果。
    for (auto& result : agg.results) 
    {
        if (result.model_id == output.result.model_id) 
        {
            result = std::move(output.result);
            ReleaseFrame(output.frame);
            return;
        }
    }

    agg.results.push_back(std::move(output.result));
    if (!first_result) 
    {
        ReleaseFrame(output.frame);
    }
}

void FrameResultAggregator::PublishReadyLocked()
{
    PublishAvailableLocked();
}

void FrameResultAggregator::PublishTimeoutLocked()
{
    PublishAvailableLocked();
}

void FrameResultAggregator::PublishAvailableLocked()
{
    auto now = std::chrono::steady_clock::now();

    for (auto& expected : expected_publish_frame_) {
        const int channel_id = expected.first;

        while (true) {
            FrameKey key;
            key.channel_id = channel_id;
            key.frame_id = expected.second;

            auto it = pending_.find(key);
            if (it == pending_.end()) {
                break;
            }

            const bool ready = HasRequiredResultsLocked(it->second);
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.first_seen);
            const bool timed_out = waited >= timeout_;

            if (!ready && !timed_out) {
                break;
            }

            ComposedFrame frame = MakeComposedFrame(it->second, !ready);
            auto cb = on_frame_ready_;
            RememberFinishedLocked(key);
            pending_.erase(it);
            expected.second++;

            if (cb) {
                cb(frame);
            } else {
                ReleaseFrame(frame.frame);
            }
        }
    }
}

bool FrameResultAggregator::HasRequiredResultsLocked(const FrameAggregate& agg) const
{
    if (required_models_.empty()) 
    {
        return true;
    }

    for (const auto& required : required_models_) 
    {
        bool found = false;

        for (const auto& result : agg.results) 
        {
            if (result.model_id == required && result.ok) 
            {
                found = true;
                break;
            }
        }

        if (!found) 
        {
            return false;
        }
    }

    return true;
}

ComposedFrame FrameResultAggregator::MakeComposedFrame(const FrameAggregate& agg, bool partial) const
{
    ComposedFrame frame;
    frame.frame = agg.frame;
    frame.results = agg.results;
    frame.partial = partial;

    for (const auto& required : required_models_) 
    {
        bool found = false;

        for (const auto& result : agg.results) 
        {
            if (result.model_id == required && result.ok) 
            {
                found = true;
                break;
            }
        }

        if (!found) 
        {
            frame.missing_models.push_back(required);
        }
    }

    return frame;
}
