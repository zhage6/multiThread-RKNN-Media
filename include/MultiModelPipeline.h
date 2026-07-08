#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "IModelAdapter.h"

class FrameResultAggregator;

class MultiModelPipeline
{
public:
    MultiModelPipeline();
    void AddModel(IModelAdapter* model, uint32_t frame_interval = 1);
    void SetAggregator(FrameResultAggregator* aggregator);

    bool Submit(const FrameContext& frame);

    bool TryGet(ModelOutput& output);

    size_t PendingCount() const;

private:
    void PushCompleted(ModelOutput output);

private:
    struct ModelEntry {
        IModelAdapter* model = nullptr;
        uint32_t frame_interval = 1;
    };

    std::vector<ModelEntry> models_;
    FrameResultAggregator* aggregator_ = nullptr;
    mutable std::mutex completed_mtx_;
    std::deque<ModelOutput> completed_outputs_;
};
