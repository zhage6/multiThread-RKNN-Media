#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rockchip/mpp_buffer.h>
#include "postprocess.h"
#include "rkYolov5s.hpp"

using ModelId = std::string;

struct FrameContext 
{
    int channel_id = -1;
    uint64_t frame_id = 0;
    int64_t pts_us = -1;

    int src_fd = -1;
    MppBuffer src_buffer = nullptr;

    int width = 0;
    int height = 0;
    int hor_stride = 0;
    int ver_stride = 0;
};

enum class ModelResultType 
{
    Detection,
    Classification,
    Segmentation,
    Keypoints,
    Custom
};

struct ModelResult 
{
    ModelId model_id;
    ModelResultType type = ModelResultType::Custom;

    bool ok = false;
    std::string error;
    uint64_t inference_us = 0;

    // 第一阶段先兼容 YOLO 检测结果。
    detect_result_group_t detections {};
};

struct ComposedFrame 
{
    FrameContext frame;

    std::vector<ModelResult> results;

    bool partial = false;
    std::vector<ModelId> missing_models;
};

inline ComposedFrame MakeYoloComposedFrame(const InferOutput& out)
{
    ComposedFrame composed;

    composed.frame.channel_id = out.channel_id;
    composed.frame.frame_id = out.frame_id;
    composed.frame.pts_us = out.pts_us;
    composed.frame.src_fd = out.src_fd;
    composed.frame.src_buffer = out.src_buffer;
    composed.frame.width = out.width;
    composed.frame.height = out.height;
    composed.frame.hor_stride = out.hor_stride;
    composed.frame.ver_stride = out.ver_stride;

    ModelResult result;
    result.model_id = "yolo";
    result.type = ModelResultType::Detection;
    result.ok = true;
    result.detections = out.results;

    composed.results.push_back(result);
    composed.partial = false;

    return composed;
}