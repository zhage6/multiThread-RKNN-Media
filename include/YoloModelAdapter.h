#pragma once

#include "IModelAdapter.h"
#include "TimingLogger.h"
#include "rknnPool.hpp"
#include "rkYolov5s.hpp"
#include <utility>

class YoloModelAdapter : public IModelAdapter
{
public:
    explicit YoloModelAdapter(ModelId model_id,rknnPool<rkYolov5s, input_data, InferOutput>* pool)
        : model_id_(std::move(model_id)),
          pool_(pool)
    {
    }

    const ModelId& ModelName() const override
    {
        return model_id_;
    }

    ModelResultType ResultType() const override
    {
        return ModelResultType::Detection;
    }

    bool Submit(const FrameContext& frame) override
    {
        return Submit(frame, ModelOutputCallback());
    }

    bool Submit(const FrameContext& frame, ModelOutputCallback cb) override
    {
        if (!pool_) {
            return false;
        }

        input_data data;
        data.src_fd = frame.src_fd;
        data.src_buffer = frame.src_buffer;
        data.width = frame.width;
        data.height = frame.height;
        data.hor_stride = frame.hor_stride;
        data.ver_stride = frame.ver_stride;
        data.frame = nullptr;
        data.channel_id = frame.channel_id;
        data.frame_id = frame.frame_id;
        data.pts_us = frame.pts_us;

        size_t pending_before = PendingCount();
        bool ok = false;
        if (cb) 
        {
            ok = pool_->put(data, [this, cb = std::move(cb)](InferOutput infer_out) mutable
            { 
                ModelOutput output = ConvertOutput(std::move(infer_out));
                timing::Log("model_output model=%s ch=%d frame=%llu pending_after=%zu boxes=%d",
                            model_id_.c_str(),
                            output.frame.channel_id,
                            static_cast<unsigned long long>(output.frame.frame_id),
                            PendingCount(),
                            output.result.detections.count);
                cb(std::move(output));
            }) == 0;
        } 
        else 
        {
            ok = pool_->put(data) == 0;
        }
        size_t pending_after = PendingCount();

        timing::Log("model_submit model=%s ch=%d frame=%llu ok=%d pending_before=%zu pending_after=%zu",
                    model_id_.c_str(),
                    frame.channel_id,
                    static_cast<unsigned long long>(frame.frame_id),
                    ok ? 1 : 0,
                    pending_before,
                    pending_after);

        return ok;
    }

    bool TryGet(ModelOutput& output) override
    {
        if (!pool_) {
            return false;
        }

        InferOutput infer_out;
        if (pool_->get(infer_out) != 0) {
            return false;
        }

        output = ConvertOutput(std::move(infer_out));

        timing::Log("model_output model=%s ch=%d frame=%llu pending_after=%zu boxes=%d",
                    model_id_.c_str(),
                    output.frame.channel_id,
                    static_cast<unsigned long long>(output.frame.frame_id),
                    PendingCount(),
                    output.result.detections.count);

        return true;
    }

    size_t PendingCount() const override
    {
        if (!pool_) {
            return 0;
        }

        return static_cast<size_t>(pool_->get_task_size());
    }

private:
    ModelOutput ConvertOutput(InferOutput infer_out) const
    {
        ModelOutput output;
        output.frame.channel_id = infer_out.channel_id;
        output.frame.frame_id = infer_out.frame_id;
        output.frame.pts_us = infer_out.pts_us;
        output.frame.src_fd = infer_out.src_fd;
        output.frame.src_buffer = infer_out.src_buffer;
        output.frame.width = infer_out.width;
        output.frame.height = infer_out.height;
        output.frame.hor_stride = infer_out.hor_stride;
        output.frame.ver_stride = infer_out.ver_stride;

        output.result.model_id = model_id_;
        output.result.type = ModelResultType::Detection;
        output.result.ok = true;
        output.result.detections = infer_out.results;

        return output;
    }

    ModelId model_id_;
    rknnPool<rkYolov5s, input_data, InferOutput>* pool_ = nullptr;
};
