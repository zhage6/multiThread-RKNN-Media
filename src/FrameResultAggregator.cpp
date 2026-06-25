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
    queue_.clear();
    pending_.clear();
}

bool FrameResultAggregator::Submit(ModelOutput output)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (!running_) {
        return false;
    }

    queue_.push_back(std::move(output));
    cv_.notify_one();
    return true;
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

    auto& agg = pending_[key];

    if (agg.results.empty()) 
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
            return;
        }
    }

    agg.results.push_back(std::move(output.result));
}

void FrameResultAggregator::PublishReadyLocked()
{
    for (auto it = pending_.begin(); it != pending_.end(); ) 
    {
        if (HasRequiredResultsLocked(it->second)) 
        {
            ComposedFrame frame = MakeComposedFrame(it->second, false);
            auto cb = on_frame_ready_;
            it = pending_.erase(it);

            if (cb) 
            {
                cb(frame);
            }
        } 
        else 
        {
            ++it;
        }
    }
}

void FrameResultAggregator::PublishTimeoutLocked()
{
    auto now = std::chrono::steady_clock::now();

    for (auto it = pending_.begin(); it != pending_.end(); ) {
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.first_seen);

        if (waited >= timeout_) {
            ComposedFrame frame = MakeComposedFrame(it->second, true);
            auto cb = on_frame_ready_;
            it = pending_.erase(it);

            if (cb) {
                cb(frame);
            }
        } else {
            ++it;
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