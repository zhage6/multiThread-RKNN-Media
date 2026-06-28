#pragma once

#include "IModelAdapter.h"
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

        return pool_->put(data) == 0;
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
    ModelId model_id_;
    rknnPool<rkYolov5s, input_data, InferOutput>* pool_ = nullptr;
};