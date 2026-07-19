#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "IModelAdapter.h"

class FrameResultAggregator;
using InvalidModelOutputCallback = std::function<void(int channel_id)>;

class MultiModelPipeline
{
public:
    MultiModelPipeline();
    void AddModel(IModelAdapter* model, uint32_t frame_interval = 1);
    void SetAggregator(FrameResultAggregator* aggregator);
    void SetInvalidOutputCallback(InvalidModelOutputCallback cb);

    bool Submit(const FrameContext& frame);

    size_t PendingCount() const;
    size_t DrainPendingCount() const;

private:
    void ForwardCompleted(ModelOutput output);

private:
    struct ModelEntry {
        IModelAdapter* model = nullptr;
        uint32_t frame_interval = 1;
    };

    std::vector<ModelEntry> models_;
    FrameResultAggregator* aggregator_ = nullptr;
    InvalidModelOutputCallback invalid_output_callback_;
};
