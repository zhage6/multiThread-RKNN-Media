#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <set>

#include "FrameTypes.h"

using AggregatedFrameCallback = std::function<void(const ComposedFrame&)>;
using DroppedFrameCallback = std::function<void(int channel_id, uint64_t frame_id)>;

class FrameResultAggregator 
{
public:
    FrameResultAggregator();
    ~FrameResultAggregator();

    void SetRequiredModels(std::vector<ModelId> models);
    void SetTimeout(std::chrono::milliseconds timeout);
    void SetOutputCallback(AggregatedFrameCallback cb);
    void SetDropCallback(DroppedFrameCallback cb);

    bool Start();
    void Stop();

    void RegisterExpectedModels(const FrameContext& frame, std::vector<ModelId> models);
    void CancelExpectedFrame(const FrameContext& frame);
    bool Submit(ModelOutput output);
    size_t PendingCount() const;

private:
    struct FrameAggregate 
    {
        FrameContext frame;
        std::vector<ModelResult> results;
        std::chrono::steady_clock::time_point first_seen;
    };

    struct LockStatsSnapshot
    {
        uint64_t submit_count = 0;
        uint64_t submit_total_us = 0;
        uint64_t submit_max_us = 0;
        uint64_t submit_over_500us = 0;
        uint64_t worker_count = 0;
        uint64_t worker_total_us = 0;
        uint64_t worker_max_us = 0;
        uint64_t worker_over_2ms = 0;
        size_t queue_size = 0;
        size_t pending_size = 0;
        size_t registered_size = 0;
    };

    struct PublishBatch
    {
        AggregatedFrameCallback on_frame_ready;
        DroppedFrameCallback on_frame_dropped;
        std::vector<ComposedFrame> ready_frames;
        std::vector<FrameKey> dropped_frames;
    };
    void ReleaseFrame(FrameContext& frame);
    void ReleaseOutput(ModelOutput& output);
    void ReleaseAggregate(FrameAggregate& agg); //释放资源

    bool IsFinishedLocked(const FrameKey& key) const;
    void RememberFinishedLocked(const FrameKey& key);

    void WorkerLoop();
    void AddResult(ModelOutput output);
    void PublishAvailableLocked(PublishBatch& batch);
    void DispatchPublishBatch(PublishBatch& batch);
    const std::vector<ModelId>& ExpectedModelsLocked(const FrameKey& key) const;
    bool HasRequiredResultsLocked(const FrameKey& key, const FrameAggregate& agg) const;
    ComposedFrame MakeComposedFrame(const FrameKey& key, const FrameAggregate& agg, bool partial) const;
    void RecordSubmitLockWait(long long wait_us);
    void RecordWorkerLockHold(long long hold_us);
    bool TakeLockStatsLocked(LockStatsSnapshot& stats);
    void LogLockStats(const LockStatsSnapshot& stats) const;

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ModelOutput> queue_;
    std::map<FrameKey, FrameAggregate> pending_;//按 channel_id + frame_id 暂存同一帧的多个模型结果
    std::map<FrameKey, std::vector<ModelId>> expected_models_;
    // 已成功投递给至少一个模型、但尚未收到任何结果的帧，从注册时刻开始计时。
    std::map<FrameKey, std::chrono::steady_clock::time_point> registered_since_;

    std::vector<ModelId> required_models_;
    AggregatedFrameCallback on_frame_ready_;
    DroppedFrameCallback on_frame_dropped_;

    std::thread worker_;
    bool running_ = false;
    std::chrono::milliseconds timeout_{120};

    std::set<FrameKey> finished_frames_;
    std::deque<FrameKey> finished_order_;
    size_t max_finished_history_ = 1024;

    // These counters expose whether direct worker -> aggregator handoff is lock-bound.
    std::atomic<uint64_t> submit_lock_wait_count_{0};
    std::atomic<uint64_t> submit_lock_wait_total_us_{0};
    std::atomic<uint64_t> submit_lock_wait_max_us_{0};
    std::atomic<uint64_t> submit_lock_wait_over_500us_{0};
    std::atomic<uint64_t> worker_lock_hold_count_{0};
    std::atomic<uint64_t> worker_lock_hold_total_us_{0};
    std::atomic<uint64_t> worker_lock_hold_max_us_{0};
    std::atomic<uint64_t> worker_lock_hold_over_2ms_{0};
    std::chrono::steady_clock::time_point last_lock_stats_log_{};
};
