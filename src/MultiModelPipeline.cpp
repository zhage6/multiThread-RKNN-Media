#include "MultiModelPipeline.h"
#include "FrameResultAggregator.h"

#include <algorithm>
#include <rockchip/mpp_buffer.h>

MultiModelPipeline::MultiModelPipeline()
{
}

void MultiModelPipeline::AddModel(IModelAdapter* model, uint32_t frame_interval)
{
    if (model) {
        ModelEntry entry;
        entry.model = model;
        entry.frame_interval = std::max(1u, frame_interval);
        models_.push_back(entry);
    }
}

void MultiModelPipeline::SetAggregator(FrameResultAggregator* aggregator)
{
    aggregator_ = aggregator;
}

void MultiModelPipeline::SetInvalidOutputCallback(InvalidModelOutputCallback cb)
{
    invalid_output_callback_ = std::move(cb);
}

bool MultiModelPipeline::Submit(const FrameContext& frame)
{
    if (models_.empty()) 
    {
        return false;
    }

    std::vector<ModelEntry*> selected_models;
    std::vector<ModelId> expected_models;

    for (auto& entry : models_) 
    {
        IModelAdapter* model = entry.model;
        if (!model) 
        {
            continue;
        }

        if (entry.frame_interval > 1 &&
            frame.frame_id % entry.frame_interval != 0) //这里直接通过帧号对模型进行间隔采样，frame_interval=1表示每帧都送去推理，frame_interval=2表示每两帧送去推理一次
        {
            continue;
        }

        selected_models.push_back(&entry);
        expected_models.push_back(model->ModelName());
    }

    if (selected_models.empty()) 
    {
        return false;
    }

    if (aggregator_) {
        aggregator_->RegisterExpectedModels(frame, expected_models);
    }

    bool any_submitted = false;

    for (auto* entry : selected_models) 
    {
        IModelAdapter* model = entry->model;
        FrameContext task_frame = frame;

        if (task_frame.src_buffer) 
        {
            mpp_buffer_inc_ref(task_frame.src_buffer);
        }

        if (model->Submit(task_frame, [this](ModelOutput output)
        {
            ForwardCompleted(std::move(output));
        })) 
        {
            any_submitted = true;
        } else 
        {
            if (task_frame.src_buffer) 
            {
                mpp_buffer_put(task_frame.src_buffer);
            }
        }
    }

    if (!any_submitted && aggregator_) {
        aggregator_->CancelExpectedFrame(frame);
    }

    return any_submitted;
}


size_t MultiModelPipeline::PendingCount() const
{
    size_t total = 0;

    for (auto& entry : models_) {
        IModelAdapter* model = entry.model;
        if (model) {
            total += model->PendingCount();
        }
    }

    return total;
}

size_t MultiModelPipeline::DrainPendingCount() const
{
    size_t total = PendingCount();
    if (aggregator_) {
        total += aggregator_->PendingCount();
    }
    return total;
}

void MultiModelPipeline::ForwardCompleted(ModelOutput output)
{
    if (output.frame.frame_id == static_cast<uint64_t>(-1))
    {
        const int channel_id = output.frame.channel_id;
        if (invalid_output_callback_) {
            invalid_output_callback_(channel_id);
        }

        if (output.frame.src_buffer) {
            mpp_buffer_put(output.frame.src_buffer);
        }
        return;
    }

    MppBuffer src_buffer = output.frame.src_buffer;
    if (!aggregator_ || !aggregator_->Submit(std::move(output))) {
        if (src_buffer) {
            mpp_buffer_put(src_buffer);
        }
    }
}
