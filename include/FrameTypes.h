#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rockchip/mpp_buffer.h>
#include "postprocess.h"

using ModelId = std::string;

struct FrameContext 
{
    int channel_id = -1;
    uint64_t frame_id = 0;
    int64_t pts_us = -1;
    int64_t origin_wall_ms = -1;

    int src_fd = -1;
    MppBuffer src_buffer = nullptr;

    int width = 0;
    int height = 0;
    int hor_stride = 0;
    int ver_stride = 0;
};

struct FrameKey 
{
    int channel_id = -1;
    uint64_t frame_id = 0;

    bool operator<(const FrameKey& other) const
    {
        if (channel_id != other.channel_id) {
            return channel_id < other.channel_id;
        }
        return frame_id < other.frame_id;
    } //<重构运算符，让frame key可以完成map的排列
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

struct ModelOutput 
{
    FrameContext frame;
    ModelResult result;
};




struct ComposedFrame 
{
    FrameContext frame;

    std::vector<ModelResult> results;

    bool partial = false;
    std::vector<ModelId> missing_models;
};
