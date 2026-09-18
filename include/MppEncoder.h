#pragma once

#include <thread>
#include <mutex>
#include <deque>
#include <functional>
#include <vector>
#include <atomic>
#include <chrono>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_packet.h>
#include "mpp_packet_impl.h"


// 定义码流回调函数的类型
// data: 编码后的 H264/H265 码流指针
// size: 码流大小
// is_keyframe: 是否是 I 帧 (方便上层做推流时的关键帧判断)
// input_submit_wall_ms: 对应输入帧提交给编码器时的系统时间
using PacketCallback = std::function<void(const uint8_t* data, size_t size,
                                          bool is_keyframe,
                                          int64_t input_submit_wall_ms)>;

class RkMppEncoder 
{
public:
    RkMppEncoder();
    ~RkMppEncoder();

    // 1. 初始化编码器 (设置宽高、像素格式、编码格式 H264/H265 等)
    bool Init(int width,
              int height,
              int hor_stride,
              int ver_stride,
              MppFrameFormat fmt,
              MppCodingType type,
              int fps = 30);

    // 2. 注册输出回调，硬件编码完成后会触发这个函数
    void SetOutputCallback(PacketCallback cb);

    // 3. 启动编码线程
    bool Start();

    bool PushBuffer(MppBuffer buffer, int64_t input_submit_wall_ms = -1);

    void RecycleBuffer(MppBuffer buffer);

    bool GetHeader(std::vector<uint8_t>& header);

    // 5. 停止并释放资源
    void Stop();

private:
    void OutputThreadFunc();
    void RecycleEncodedFrame(MppFrame frame);
    void RemovePendingFrame(MppFrame frame);
    bool RecycleOldestPendingFrame();
    int64_t PeekOldestPendingSubmitWallMs();
    void MaybeLogStats(const char* source);

private:
    struct PendingFrame {
        MppFrame frame = nullptr;
        int64_t input_submit_wall_ms = -1;
    };

    // MPP 核心上下文 (原 MpiEncTestData 中的核心成员)
    MppCtx ctx_;
    MppApi* mpi_;
    MppEncCfg cfg_;

    // 基础参数
    int width_;
    int height_;
    int hor_stride_;
    int ver_stride_;
    MppFrameFormat fmt_;
    
    // C++ 线程管理
    std::thread output_thread_;
    std::atomic<bool> is_running_;

    // 回调函数
    PacketCallback on_packet_ready_;

    std::mutex stats_mtx_;
    std::chrono::steady_clock::time_point stats_last_;
    uint64_t stats_last_in_;
    uint64_t stats_last_packet_;
    uint64_t stats_last_frame_;
    uint64_t stats_last_recycle_;
    std::atomic<uint64_t> input_frame_count_;
    std::atomic<uint64_t> output_packet_count_;
    std::atomic<uint64_t> output_frame_count_;
    std::atomic<uint64_t> recycled_frame_count_;

    std::deque<PendingFrame> pending_frames_;
    std::mutex mtx_;
};
