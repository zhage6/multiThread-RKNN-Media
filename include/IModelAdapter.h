#pragma once

#include <cstddef>
#include <functional>

#include "FrameTypes.h"

using ModelOutputCallback = std::function<void(ModelOutput)>;

class IModelAdapter
{
public:
    virtual ~IModelAdapter() = default;

    virtual const ModelId& ModelName() const = 0;
    virtual ModelResultType ResultType() const = 0;
     // Submit 成功后，adapter 接管 frame.src_buffer 这一份引用。
    // Submit 失败时，adapter 必须释放 frame.src_buffer，或者调用方负责释放，二者只能选一个。
    virtual bool Submit(const FrameContext& frame) = 0;
    virtual bool Submit(const FrameContext& frame, ModelOutputCallback cb) = 0;

    // 非阻塞取结果。拿到的是统一的 ModelOutput。
    virtual bool TryGet(ModelOutput& output) = 0;

    virtual size_t PendingCount() const = 0;
};
