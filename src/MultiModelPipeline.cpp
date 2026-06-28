#include "MultiModelPipeline.h"

#include <rockchip/mpp_buffer.h>

MultiModelPipeline::MultiModelPipeline()
{
}

void MultiModelPipeline::AddModel(IModelAdapter* model)
{
    if (model) {
        models_.push_back(model);
    }
}

bool MultiModelPipeline::Submit(const FrameContext& frame)
{
    if (models_.empty()) {
        return false;
    }

    bool any_submitted = false;

    for (auto* model : models_) {
        if (!model) {
            continue;
        }

        FrameContext task_frame = frame;

        if (task_frame.src_buffer) {
            mpp_buffer_inc_ref(task_frame.src_buffer);
        }

        if (model->Submit(task_frame)) {
            any_submitted = true;
        } else {
            if (task_frame.src_buffer) {
                mpp_buffer_put(task_frame.src_buffer);
            }
        }
    }

    return any_submitted;
}


size_t MultiModelPipeline::PendingCount() const
{
    size_t total = 0;

    for (auto* model : models_) {
        if (model) {
            total += model->PendingCount();
        }
    }

    return total;
}
bool MultiModelPipeline::TryGet(ModelOutput& output)
{
    for (auto* model : models_) 
    {
        if (model && model->TryGet(output)) 
        {
            return true;
        }
    }
    return false;
}