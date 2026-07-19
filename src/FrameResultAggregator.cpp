#include "FrameResultAggregator.h"
#include "TimingLogger.h"

#include <string>

namespace
{
    std::string ResultModelsString(const std::vector<ModelResult>& results)
    {
        std::string models;
        for (const auto& result : results) {
            if (!models.empty()) {
                models += ",";
            }
            models += result.model_id;
        }
        return models;
    }
}

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

void FrameResultAggregator::SetDropCallback(DroppedFrameCallback cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    on_frame_dropped_ = std::move(cb);
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
    expected_models_.clear();
    registered_since_.clear();
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
        timing::Log("agg_submit_finished_drop model=%s ch=%d frame=%llu boxes=%d",
                    output.result.model_id.c_str(),
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id),
                    output.result.detections.count);
        ReleaseOutput(output);
        return true;
    }

    queue_.push_back(std::move(output));
    cv_.notify_one();
    return true;
}

void FrameResultAggregator::RegisterExpectedModels(const FrameContext& frame, std::vector<ModelId> models)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (models.empty()) {
        return;
    }

    FrameKey key;
    key.channel_id = frame.channel_id;
    key.frame_id = frame.frame_id;

    if (IsFinishedLocked(key)) {
        return;
    }

    expected_models_[key] = std::move(models);
    if (registered_since_.find(key) == registered_since_.end()) {
        registered_since_.emplace(key, std::chrono::steady_clock::now());
    }
    cv_.notify_one();
}

void FrameResultAggregator::CancelExpectedFrame(const FrameContext& frame)
{
    std::lock_guard<std::mutex> lock(mtx_);

    FrameKey key;
    key.channel_id = frame.channel_id;
    key.frame_id = frame.frame_id;

    if (pending_.find(key) != pending_.end()) {
        return;
    }

    expected_models_.erase(key);
    registered_since_.erase(key);
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
            else //如果队列是空的
            {
                PublishTimeoutLocked(); //先检查有没有超时
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
        timing::Log("agg_late_drop model=%s ch=%d frame=%llu reason=finished boxes=%d",
                    output.result.model_id.c_str(),
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id),
                    output.result.detections.count);
        ReleaseOutput(output);
        return;
    }

    auto& agg = pending_[key]; //查找聚合帧map中有没有对应的那一帧
    bool first_result = agg.results.empty();//如果是模型的某一帧中的第一帧结果则记上时间，方便后续做超时计算
    if (first_result) 
    {
        agg.frame = output.frame;
        agg.first_seen = std::chrono::steady_clock::now();
        registered_since_.erase(key);
        timing::Log("agg_first_result model=%s ch=%d frame=%llu",
                    output.result.model_id.c_str(),
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id));
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
    timing::Log("agg_add model=%s ch=%d frame=%llu results=%zu boxes=%d first=%d",
                agg.results.back().model_id.c_str(),
                key.channel_id,
                static_cast<unsigned long long>(key.frame_id),
                agg.results.size(),
                agg.results.back().detections.count,
                first_result ? 1 : 0);
    if (!first_result) //如果再来第二个模型的应该释放一次模型引用，因为在推理前，每一帧都加了二次引用(不同模型)！
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

    for (auto it = pending_.begin(); it != pending_.end();)
    {
        const FrameKey key = it->first;
        const bool ready = HasRequiredResultsLocked(key, it->second);
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.first_seen);
        const bool timed_out = waited >= timeout_;

        if (!ready && !timed_out)
        {
            ++it;
            continue;
        }

        ComposedFrame frame = MakeComposedFrame(key, it->second, !ready);
        const std::string models = ResultModelsString(it->second.results);
        bool has_face = false;
        int face_boxes = 0;
        int yolo_boxes = 0;
        for (const auto& result : frame.results) {
            if (result.model_id == "face_yolo") {
                has_face = true;
                face_boxes = result.detections.count;
            } else if (result.model_id == "yolo") {
                yolo_boxes = result.detections.count;
            }
        }
        timing::Log("agg_publish ch=%d frame=%llu partial=%d ready=%d results=%zu models=%s has_face=%d face_boxes=%d yolo_boxes=%d waited_ms=%lld",
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id),
                    frame.partial ? 1 : 0,
                    ready ? 1 : 0,
                    frame.results.size(),
                    models.c_str(),
                    has_face ? 1 : 0,
                    face_boxes,
                    yolo_boxes,
                    static_cast<long long>(waited.count()));
        auto cb = on_frame_ready_;
        RememberFinishedLocked(key);
        expected_models_.erase(key);
        it = pending_.erase(it);

        if (cb) {
            cb(frame);
        } else {
            ReleaseFrame(frame.frame);
        }
    }

    for (auto it = registered_since_.begin(); it != registered_since_.end();)
    {
        const FrameKey key = it->first;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second);
        if (waited < timeout_)
        {
            ++it;
            continue;
        }

        auto drop_cb = on_frame_dropped_;
        timing::Log("agg_no_result_drop ch=%d frame=%llu waited_ms=%lld",
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id),
                    static_cast<long long>(waited.count()));
        RememberFinishedLocked(key);
        expected_models_.erase(key);
        it = registered_since_.erase(it);

        if (drop_cb) {
            drop_cb(key.channel_id, key.frame_id);
        }
    }
}

const std::vector<ModelId>& FrameResultAggregator::ExpectedModelsLocked(const FrameKey& key) const
{
    auto expected = expected_models_.find(key);
    if (expected != expected_models_.end()) {
        return expected->second;
    }

    return required_models_;
}

bool FrameResultAggregator::HasRequiredResultsLocked(const FrameKey& key, const FrameAggregate& agg) const
{
    const auto& required_models = ExpectedModelsLocked(key);
    if (required_models.empty()) 
    {
        return true;
    }

    for (const auto& required : required_models) 
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

ComposedFrame FrameResultAggregator::MakeComposedFrame(const FrameKey& key, const FrameAggregate& agg, bool partial) const
{
    ComposedFrame frame;
    frame.frame = agg.frame;
    frame.results = agg.results;
    frame.partial = partial;

    const auto& required_models = ExpectedModelsLocked(key);
    for (const auto& required : required_models) 
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
