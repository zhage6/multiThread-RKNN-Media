#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

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

private:
    struct FrameAggregate 
    {
        FrameContext frame;
        std::vector<ModelResult> results;
        std::chrono::steady_clock::time_point first_seen;
    };

    void WorkerLoop();
    void AddResult(ModelOutput output);
    void PublishReadyLocked();
    void PublishTimeoutLocked();
    bool HasRequiredResultsLocked(const FrameAggregate& agg) const;
    ComposedFrame MakeComposedFrame(const FrameAggregate& agg, bool partial) const;

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ModelOutput> queue_;
    std::map<FrameKey, FrameAggregate> pending_;

    std::vector<ModelId> required_models_;
    AggregatedFrameCallback on_frame_ready_;

    std::thread worker_;
    bool running_ = false;
    std::chrono::milliseconds timeout_{120};
};