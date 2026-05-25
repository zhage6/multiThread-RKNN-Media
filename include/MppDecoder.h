#ifndef __MPPDECODER_H
#define __MPPDECODER_H

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <functional>

// 瑞芯微硬件加速核心头文件
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_frame.h>

// RGA 2D 加速头文件
#include "im2d.hpp"
#include "rga.h"

// 自定义 DMA 内存分配器
#include "dma_alloc.h"

#define MPI_DEC_STREAM_SIZE         1024*600

// 外部 DMA 缓冲区结构体：用于追踪分配的 24 块输入画布
struct ExternalBuffer {
    void* src_buf;             // CPU 虚拟地址（调试或必要时使用）
    int    src_dma_fd;          // 核心：物理内存的 fd 句柄
    size_t buf_size;            // 内存大小
    rga_buffer_handle_t rga_handle; // RGA 认识的内存句柄
    rga_buffer_t rga_buf;       // RGA 认识的图像结构体
};

class MppDecoder {
public:
    // 定义一个标准 C++ 回调函数
    // 当解出一帧 YUV 图像时，直接把它的物理 fd 以及高宽步长信息抛给外层
    using FrameCallback = std::function<void(int src_fd, int width, int height, int hor_stride, int ver_stride)>;

    MppDecoder();
    ~MppDecoder();

    // 1. 初始化解码器环境 (传入回调函数与视频格式，默认 H.264)
    bool Init(FrameCallback callback, MppCodingType type = MPP_VIDEO_CodingAVC);

    // 2. 核心数据输入接口：无论是读本地文件还是网络推流，拿到 H264 裸流直接往这里喂
    void DecodePacket(const uint8_t* data, size_t size);

private:
    // 内部处理：处理硬件解码循环
    void FlushDecoder();
    
    // 内部处理：动态分配 24 块外部 DMA 内存池（Mode 3 核心）
    int AllocateExternalBuffers(size_t buf_size, int width, int height);
    
    // 释放所有硬件与内存资源
    void ReleaseAll();

private:
    FrameCallback m_callback; // 外部绑定的回调函数

    // MPP 核心句柄
    MppCtx          m_ctx;
    MppApi* m_mpi;
    MppPacket       m_packet;
    MppBufferGroup  m_frm_grp; // 外部内存组管理其

    // 状态控制
    MppCodingType   m_type;
    bool            m_initialized;
    
    // 视频流实时属性
    int             m_src_width;
    int             m_src_height;
    int             m_hor_stride;
    int             m_ver_stride;

public:
    // 缓存映射表：通过 fd 快速查找对应的 RGA 输入缓冲结构体
    std::map<int, ExternalBuffer> buffer_map;
};

#endif // __MPPDECODER_H