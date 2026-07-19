#pragma once

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

private:
    struct FrameAggregate 
    {
        FrameContext frame;
        std::vector<ModelResult> results;
        std::chrono::steady_clock::time_point first_seen;
    };
    void ReleaseFrame(FrameContext& frame);
    void ReleaseOutput(ModelOutput& output);
    void ReleaseAggregate(FrameAggregate& agg); //释放资源

    bool IsFinishedLocked(const FrameKey& key) const;
    void RememberFinishedLocked(const FrameKey& key);

    void WorkerLoop();
    void AddResult(ModelOutput output);
    void PublishReadyLocked();
    void PublishTimeoutLocked();
    void PublishAvailableLocked();
    const std::vector<ModelId>& ExpectedModelsLocked(const FrameKey& key) const;
    bool HasRequiredResultsLocked(const FrameKey& key, const FrameAggregate& agg) const;
    ComposedFrame MakeComposedFrame(const FrameKey& key, const FrameAggregate& agg, bool partial) const;

private:
    std::mutex mtx_;
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
};
