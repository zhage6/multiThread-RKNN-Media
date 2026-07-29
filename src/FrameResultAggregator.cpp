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
    const auto lock_wait_start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mtx_);
    RecordSubmitLockWait(timing::UsSince(lock_wait_start));

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

size_t FrameResultAggregator::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return queue_.size() + pending_.size() + registered_since_.size();
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
        bool has_output = false;
        LockStatsSnapshot lock_stats;
        bool should_log_lock_stats = false;
        PublishBatch publish_batch;

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
                has_output = true;
            }

            const auto lock_hold_start = std::chrono::steady_clock::now();
            publish_batch.on_frame_ready = on_frame_ready_;
            publish_batch.on_frame_dropped = on_frame_dropped_;

            if (has_output) {
                AddResult(std::move(output));
            }
            PublishAvailableLocked(publish_batch);

            RecordWorkerLockHold(timing::UsSince(lock_hold_start));
            should_log_lock_stats = TakeLockStatsLocked(lock_stats);
        }

        if (should_log_lock_stats) {
            LogLockStats(lock_stats);
        }
        DispatchPublishBatch(publish_batch);
    }
}

void FrameResultAggregator::RecordSubmitLockWait(long long wait_us)
{
    const uint64_t value = static_cast<uint64_t>(std::max(0LL, wait_us));
    submit_lock_wait_count_.fetch_add(1, std::memory_order_relaxed);
    submit_lock_wait_total_us_.fetch_add(value, std::memory_order_relaxed);
    if (value >= 500) {
        submit_lock_wait_over_500us_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t previous = submit_lock_wait_max_us_.load(std::memory_order_relaxed);
    while (previous < value &&
           !submit_lock_wait_max_us_.compare_exchange_weak(
               previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void FrameResultAggregator::RecordWorkerLockHold(long long hold_us)
{
    const uint64_t value = static_cast<uint64_t>(std::max(0LL, hold_us));
    worker_lock_hold_count_.fetch_add(1, std::memory_order_relaxed);
    worker_lock_hold_total_us_.fetch_add(value, std::memory_order_relaxed);
    if (value >= 2000) {
        worker_lock_hold_over_2ms_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t previous = worker_lock_hold_max_us_.load(std::memory_order_relaxed);
    while (previous < value &&
           !worker_lock_hold_max_us_.compare_exchange_weak(
               previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

bool FrameResultAggregator::TakeLockStatsLocked(LockStatsSnapshot& stats)
{
    const auto now = std::chrono::steady_clock::now();
    if (last_lock_stats_log_.time_since_epoch().count() != 0 &&
        now - last_lock_stats_log_ < std::chrono::seconds(1)) {
        return false;
    }
    last_lock_stats_log_ = now;

    stats.submit_count = submit_lock_wait_count_.exchange(0, std::memory_order_relaxed);
    stats.submit_total_us = submit_lock_wait_total_us_.exchange(0, std::memory_order_relaxed);
    stats.submit_max_us = submit_lock_wait_max_us_.exchange(0, std::memory_order_relaxed);
    stats.submit_over_500us = submit_lock_wait_over_500us_.exchange(0, std::memory_order_relaxed);
    stats.worker_count = worker_lock_hold_count_.exchange(0, std::memory_order_relaxed);
    stats.worker_total_us = worker_lock_hold_total_us_.exchange(0, std::memory_order_relaxed);
    stats.worker_max_us = worker_lock_hold_max_us_.exchange(0, std::memory_order_relaxed);
    stats.worker_over_2ms = worker_lock_hold_over_2ms_.exchange(0, std::memory_order_relaxed);
    stats.queue_size = queue_.size();
    stats.pending_size = pending_.size();
    stats.registered_size = registered_since_.size();
    return true;
}

void FrameResultAggregator::LogLockStats(const LockStatsSnapshot& stats) const
{
    timing::Log(
        "agg_lock_health submit_count=%llu submit_wait_avg_us=%llu submit_wait_max_us=%llu submit_wait_over_500us=%llu worker_count=%llu worker_hold_avg_us=%llu worker_hold_max_us=%llu worker_hold_over_2ms=%llu queue=%zu pending=%zu registered=%zu",
        static_cast<unsigned long long>(stats.submit_count),
        static_cast<unsigned long long>(stats.submit_count == 0 ? 0 : stats.submit_total_us / stats.submit_count),
        static_cast<unsigned long long>(stats.submit_max_us),
        static_cast<unsigned long long>(stats.submit_over_500us),
        static_cast<unsigned long long>(stats.worker_count),
        static_cast<unsigned long long>(stats.worker_count == 0 ? 0 : stats.worker_total_us / stats.worker_count),
        static_cast<unsigned long long>(stats.worker_max_us),
        static_cast<unsigned long long>(stats.worker_over_2ms),
        stats.queue_size,
        stats.pending_size,
        stats.registered_size);
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

void FrameResultAggregator::PublishAvailableLocked(PublishBatch& batch)
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
        RememberFinishedLocked(key);
        expected_models_.erase(key);
        it = pending_.erase(it);
        batch.ready_frames.push_back(std::move(frame));
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

        timing::Log("agg_no_result_drop ch=%d frame=%llu waited_ms=%lld",
                    key.channel_id,
                    static_cast<unsigned long long>(key.frame_id),
                    static_cast<long long>(waited.count()));
        RememberFinishedLocked(key);
        expected_models_.erase(key);
        it = registered_since_.erase(it);
        batch.dropped_frames.push_back(key);
    }
}

void FrameResultAggregator::DispatchPublishBatch(PublishBatch& batch)
{
    for (auto& frame : batch.ready_frames)
    {
        if (batch.on_frame_ready) {
            batch.on_frame_ready(frame);
        } else {
            ReleaseFrame(frame.frame);
        }
    }

    if (!batch.on_frame_dropped) {
        return;
    }

    for (const auto& key : batch.dropped_frames)
    {
        batch.on_frame_dropped(key.channel_id, key.frame_id);
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
