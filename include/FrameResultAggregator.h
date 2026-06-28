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

class FrameResultAggregator 
{
public:
    FrameResultAggregator();
    ~FrameResultAggregator();

    void SetRequiredModels(std::vector<ModelId> models);
    void SetTimeout(std::chrono::milliseconds timeout);
    void SetOutputCallback(AggregatedFrameCallback cb);

    bool Start();
    void Stop();

    bool Submit(ModelOutput output);
    void SkipFrame(int channel_id, uint64_t frame_id);

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
    bool HasRequiredResultsLocked(const FrameAggregate& agg) const;
    ComposedFrame MakeComposedFrame(const FrameAggregate& agg, bool partial) const;

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ModelOutput> queue_;
    std::map<FrameKey, FrameAggregate> pending_;
    std::map<int, uint64_t> expected_publish_frame_;

    std::vector<ModelId> required_models_;
    AggregatedFrameCallback on_frame_ready_;

    std::thread worker_;
    bool running_ = false;
    std::chrono::milliseconds timeout_{120};

    std::set<FrameKey> finished_frames_;
    std::deque<FrameKey> finished_order_;
    size_t max_finished_history_ = 1024;
};
