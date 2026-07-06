#ifndef MOSAIC_COMPOSER_H
#define MOSAIC_COMPOSER_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <memory>
#include <chrono>
#include <algorithm>
#include <thread>
#include <map>
#include <rockchip/mpp_buffer.h>
#include "rkYolov5s.hpp"
#include <vector>
#include <deque>
#include "dma_alloc.h"
#include "im2d.h"
#include "RgaUtils.h"
#include "MppEncoder.h"
#include "StreamPublisher.h"
#include "FrameTypes.h"

struct MosaicInput 
{
    bool valid = false;

    int channel_id = -1;
    uint64_t frame_id = 0;
    int64_t pts_us = -1;

    MppBuffer src_buffer = nullptr;
    int src_fd = -1;

    int width = 0;
    int height = 0;
    int hor_stride = 0;
    int ver_stride = 0;

    detect_result_group_t results;
    std::vector<ModelResult> model_results;
};

struct MosaicDmaBuffer 
{
    int fd = -1;
    void* ptr = nullptr;
    size_t size = 0;
};

class MosaicComposer 
{
public:
    MosaicComposer();
    ~MosaicComposer();

    bool Init(int out_width, int out_height, int fps);
    void Submit(const InferOutput& out);
    void Submit(const ComposedFrame& frame);// 新的submit为了多模型
    void Stop();

private:
    void ReleaseInput(MosaicInput& input);
    bool ReadyLocked() const;
    void FlowLoop();
    void ComposeLocked();
    void MaybeLogStatsLocked(const char* source);

private:
    static constexpr int kChannels = 4; //固定四路输入
    bool AllocateOutputBuffers(int count);
    void ReleaseOutputBuffers();
    std::mutex mtx_;
    std::array<MosaicInput, kChannels> latest_;

    std::unique_ptr<RkMppEncoder> encoder_;
    std::unique_ptr<StreamPublisher> publisher_;

    uint64_t packet_counter_ = 0;
    int64_t last_packet_pts_ = -1;
    std::chrono::steady_clock::time_point stream_start_time_;

    int out_width_;
    int out_height_;
    int fps_;
    bool initialized_;
    int out_hor_stride_;
    int out_ver_stride_;
    size_t out_buf_size_;
    uint64_t mosaic_push_count_ = 0;
    uint64_t mosaic_tick_count_ = 0;
    uint64_t mosaic_not_ready_count_ = 0;
    uint64_t mosaic_compose_count_ = 0;
    uint64_t mosaic_busy_drop_count_ = 0;
    uint64_t mosaic_rga_fail_count_ = 0;
    uint64_t stats_last_submit_total_ = 0;
    uint64_t stats_last_tick_count_ = 0;
    uint64_t stats_last_not_ready_count_ = 0;
    uint64_t stats_last_compose_count_ = 0;
    uint64_t stats_last_push_count_ = 0;
    uint64_t stats_last_busy_drop_count_ = 0;
    std::array<uint64_t, kChannels> submit_count_ {};
    std::chrono::steady_clock::time_point stats_last_;
    std::thread flow_thread_;
    std::condition_variable flow_cv_;
    bool flow_running_ = false;
    std::array<std::map<ModelId, ModelResult>, kChannels> reusable_model_results_;

    std::vector<MosaicDmaBuffer> output_buffers_;
    MppBufferGroup mosaic_grp_ = nullptr;
};

#endif
