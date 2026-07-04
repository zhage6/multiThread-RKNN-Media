#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "IModelAdapter.h"

class MultiModelPipeline
{
public:
    MultiModelPipeline();
    void AddModel(IModelAdapter* model);

    bool Submit(const FrameContext& frame);

    bool TryGet(ModelOutput& output);

    size_t PendingCount() const;

private:
    void PushCompleted(ModelOutput output);

private:
    std::vector<IModelAdapter*> models_;
    mutable std::mutex completed_mtx_;
    std::deque<ModelOutput> completed_outputs_;
};
