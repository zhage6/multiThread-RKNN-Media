#pragma once

#include <memory>
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
    std::vector<IModelAdapter*> models_;
};