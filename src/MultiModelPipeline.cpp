#include "MultiModelPipeline.h"

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

bool MultiModelPipeline::Submit(const FrameContext& frame)
{
    if (models_.empty()) {
        return false;
    }

    bool any_submitted = false;

    for (auto& entry : models_) {
        IModelAdapter* model = entry.model;
        if (!model) {
            continue;
        }

        if (entry.frame_interval > 1 &&
            frame.frame_id % entry.frame_interval != 0) {
            continue;
        }

        FrameContext task_frame = frame;

        if (task_frame.src_buffer) 
        {
            mpp_buffer_inc_ref(task_frame.src_buffer);
        }

        if (model->Submit(task_frame, [this](ModelOutput output)
        {
            PushCompleted(std::move(output));
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

    {
        std::lock_guard<std::mutex> lock(completed_mtx_);
        total += completed_outputs_.size();
    }

    return total;
}

void MultiModelPipeline::PushCompleted(ModelOutput output)
{
    std::lock_guard<std::mutex> lock(completed_mtx_);
    completed_outputs_.push_back(std::move(output));
}

bool MultiModelPipeline::TryGet(ModelOutput& output)
{
    {
        std::lock_guard<std::mutex> lock(completed_mtx_);
        if (!completed_outputs_.empty()) 
        {
            output = std::move(completed_outputs_.front());
            completed_outputs_.pop_front();
            return true;
        }
    }

    for (auto& entry : models_)
    {
        IModelAdapter* model = entry.model;
        if (model && model->TryGet(output)) 
        {
            return true;
        }
    }

    return false;
}
