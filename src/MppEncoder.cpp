#include "MppEncoder.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <rockchip/mpp_debug.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/rk_venc_cmd.h>

#define MPP_ALIGN(x, a)         (((x) + (a) - 1) & ~((a) - 1))

RkMppEncoder::RkMppEncoder() 
    : ctx_(nullptr), 
      mpi_(nullptr), 
      cfg_(nullptr), 
      buf_grp_(nullptr), 
      is_running_(false),
      width_(0),
      height_(0),
      hor_stride_(0),
      ver_stride_(0),
      frame_size_(0),
      fmt_(MPP_FMT_YUV420SP) 
      {}

RkMppEncoder::~RkMppEncoder() 
{
    Stop();
}
bool RkMppEncoder::Init(int width, int height,int hor_stride, int ver_stride, MppFrameFormat fmt, MppCodingType type)
{
    width_ = width;
    height_ = height;
    hor_stride_ = hor_stride;
    ver_stride_ = ver_stride;
    fmt_ = fmt;
    frame_size_ = MPP_ALIGN(hor_stride_, 64) * MPP_ALIGN(ver_stride_, 64) * 3 / 2;

    MPP_RET ret = MPP_OK;
    ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) {
        printf("Encoder mpp_create 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    ret = mpp_init(ctx_, MPP_CTX_ENC, type);
    if (ret != MPP_OK) {
        printf("Encoder mpp_init 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    // 2. 初始化 cfg 对象并获取硬件默认值
    ret = mpp_enc_cfg_init(&cfg_);
    if (ret != MPP_OK) {
        printf("Encoder cfg init 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    ret = mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_);
    if (ret != MPP_OK) {
        printf("Encoder get cfg 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    // 3. 手动设置你需要的编码参数 (字典式键值对设置)
    
    // --- 准备阶段配置 (输入图像信息) ---
    mpp_enc_cfg_set_s32(cfg_, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg_, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hor_stride_);
    mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", ver_stride_);
    mpp_enc_cfg_set_s32(cfg_, "prep:format", fmt);

    // --- 码率控制配置 (RC: Rate Control) ---
    int fps_in = 30;
    int fps_out = 30;
    int bps = width * height * fps_out * 0.1 *1; // 这是一个粗略的码率估算公式，可自定义
    
    // 设置 CBR (固定码率) 或 VBR (动态码率)
    mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR); 
    
    // 设置帧率
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps_in);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps_out);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denorm", 1);
    
    // 设置码率 (bps_target 是目标码率，bps_max/min 是浮动范围)
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bps * 15 / 16);

    // --- 编码器特性配置 (Codec) ---
    // 例如设置 H.264 的 Profile，或者设置 GOP 大小 (关键帧间隔)
    mpp_enc_cfg_set_s32(cfg_, "codec:type", type);
    mpp_enc_cfg_set_s32(cfg_, "rc:gop", fps_out * 2); // 每两秒一个 I 帧 (GOP=60)

    // 4. 【最关键的一步】将配置好的 cfg 下发给硬件！
    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret != MPP_OK) 
    {
        printf("Encoder set cfg 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        printf("Encoder set header mode 失败 ret=%d\n", ret);
        Stop();
        return false;
    }

    // if (!AllocateExternalBuffers(frame_size_, 12)) {
    //     Stop();
    //     return false;
    // }

    // printf("Encoder Init Success. Width: %d, Height: %d, Stride: %d x %d\n",
    //        width_, height_, hor_stride_, ver_stride_);
    return true;
}

void RkMppEncoder::SetOutputCallback(PacketCallback cb) 
{
    on_packet_ready_ = cb;
}


bool RkMppEncoder::Start() 
{
    if (is_running_) return true;
    is_running_ = true;
    
    // 启动输出线程（负责收码流）
    output_thread_ = std::thread(&RkMppEncoder::OutputThreadFunc, this);
    
    // 注意：InputThreadFunc 这里不启动，因为我们使用 PushFrame 主动驱动
    return true;
}

bool RkMppEncoder::PushBuffer(MppBuffer buffer) 
{
    if (buffer == nullptr) 
    {
        return false;
    }

    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame) != MPP_OK || frame == nullptr) 
    {
        //RecycleBuffer(buffer);
        return false;
    }
    
    // 设置常规参数
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, hor_stride_);
    mpp_frame_set_ver_stride(frame, ver_stride_);
    mpp_frame_set_fmt(frame, fmt_); // 如 MPP_FMT_YUV420SP
    
    // 绑定已经带有画框数据的物理内存
    mpp_frame_set_buffer(frame, buffer);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        pending_frames_.push_back(frame);
    }

    MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
    if (ret != MPP_OK) {
        printf("送入编码器失败!\n");
        RemovePendingFrame(frame);
        mpp_frame_deinit(&frame);
        //RecycleBuffer(buffer);
        return false;
    }

    // frame 等输出线程收到对应 packet 后再释放并回收 buffer。
    return true;
}

void RkMppEncoder::OutputThreadFunc() 
{
    MppPacket packet = nullptr;

    while (is_running_) 
    {
        // 尝试从硬件获取码流包
        MPP_RET ret = mpi_->encode_get_packet(ctx_, &packet);
        if (ret == MPP_OK && packet != nullptr) 
        {
            // 1. 提取数据并触发回调
            void* data = mpp_packet_get_pos(packet);//获得数据指针
            size_t size = mpp_packet_get_length(packet);
            
            // 判断是否是 I 帧
            uint32_t flags = mpp_packet_get_flag(packet);
            printf("\n>>> [输出线程] 收到硬件码流包! 长度: %zu 字节, flags: 0x%08x\n", size, flags);
            bool is_keyframe = (flags & MPP_PACKET_FLAG_INTRA) ? true : false;
            
           if (size > 0 && on_packet_ready_) 
           {
                on_packet_ready_((const uint8_t*)data, size, is_keyframe);
           }

            bool frame_done = true;
            if (mpp_packet_is_partition(packet))
            {
                frame_done = mpp_packet_is_eoi(packet);
            }

            bool is_extra = (flags & MPP_PACKET_FLAG_EXTRA_DATA) ? true : false;
            if (frame_done && !is_extra && !RecycleOldestPendingFrame()) 
            {
                printf("警告：编码输出 packet 没有找到可回收的输入帧。\n");
            }

            // 销毁包描述符
            mpp_packet_deinit(&packet);
            // if (flags & MPP_PACKET_FLAG_EXTRA_DATA) 
            // {
            //     // 这是配置参数头，不需要回收 Buffer
            //     printf("收到 SPS/PPS 头信息包，正常放行。\n");
            // } 
            // else 
            // {
            //     // 正常的视频数据包，执行回收闭环
            //     MppMeta meta = mpp_packet_get_meta(packet);
            //     MppFrame orig_frame = nullptr;
                
            //     // 确保 meta 存在，且成功取到了输入帧
            //     if (meta != nullptr && MPP_OK == mpp_meta_get_frame(meta, KEY_INPUT_FRAME, &orig_frame)) 
            //     {
            //         MppBuffer used_buffer = mpp_frame_get_buffer(orig_frame);
                    
            //         {
            //             std::lock_guard<std::mutex> lock(mtx_);
            //             free_buffers_.push(used_buffer);
            //         }
            //         cv_.notify_one(); 
                    
            //         // 🚨 真正的外壳销毁地点在这里！
            //         mpp_frame_deinit(&orig_frame);
            //     } 
            //     else 
            //     {
            //         printf("警告：无法找到原始输入帧，可能导致内存泄漏！\n");
            //     }
            // }
        } 
        else 
        {
            //printf("正在等待编码完成\n");
            // 硬件还没准备好数据，休眠 1 毫秒防止 CPU 空转占满
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void RkMppEncoder::RecycleBuffer(MppBuffer buffer)
{
    if (buffer == nullptr) 
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        mpp_buffer_put(buffer);
    }
    cv_.notify_one();
}

void RkMppEncoder::RecycleEncodedFrame(MppFrame frame)
{
    if (frame == nullptr) {
        return;
    }

    MppBuffer used_buffer = mpp_frame_get_buffer(frame);
    mpp_frame_deinit(&frame);
    RecycleBuffer(used_buffer);
}

void RkMppEncoder::RemovePendingFrame(MppFrame frame)
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = pending_frames_.begin(); it != pending_frames_.end(); ++it) 
    {
        if (*it == frame) {
            pending_frames_.erase(it);
            return;
        }
    }
}

bool RkMppEncoder::RecycleOldestPendingFrame()
{
    MppFrame frame = nullptr;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (pending_frames_.empty()) {
            return false;
        }

        frame = pending_frames_.front();
        pending_frames_.pop_front();
    }

    RecycleEncodedFrame(frame);
    return true;
}

void RkMppEncoder::Stop() 
{
    // 1. 通知各线程退出
    is_running_ = false;
    cv_.notify_all(); // 唤醒可能卡在 PushFrame 等盘子的线程

    // 2. 等待硬件重置
    if (mpi_ && ctx_) {
        mpi_->reset(ctx_);
    }

    // 3. 回收输出线程
    if (output_thread_.joinable()) {
        output_thread_.join();
    }

    // 4. 清理还在编码器内部排队的输入帧，并把 group 里取出的 buffer 归还
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto frame : pending_frames_) {
            if (frame) {
                MppBuffer buffer = mpp_frame_get_buffer(frame);
                mpp_frame_deinit(&frame);
                if (buffer) {
                    mpp_buffer_put(buffer);
                }
            }
        }
        pending_frames_.clear();
    }
    ReleaseExternalBuffers();

    // 5. 销毁 MPP 各种句柄
    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }

    if (ctx_) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
    }
    
    mpi_ = nullptr;
    printf("Encoder Stopped and Resources Released.");
}

MppBuffer RkMppEncoder::GetFreeBuffer() 
{
    std::unique_lock<std::mutex> lock(mtx_);

    while (is_running_) 
    {
        MppBuffer buffer = nullptr;
        MPP_RET ret = mpp_buffer_get(buf_grp_, &buffer, frame_size_);
        if (ret == MPP_OK && buffer != nullptr) 
        {
            return buffer;
        }

        if (cv_.wait_for(lock, std::chrono::seconds(2)) == std::cv_status::timeout) 
        {
            printf("警告：编码器正在等待 MPP group 空闲输入 buffer，pending_frames=%zu ret=%d。\n",
                   pending_frames_.size(), ret);
        }
    }

    return nullptr;
}

bool RkMppEncoder::AllocateExternalBuffers(size_t frame_size, int count)
{
    std::lock_guard<std::mutex> lock(mtx_);

    MPP_RET ret = mpp_buffer_group_get_external(&buf_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret != MPP_OK || buf_grp_ == nullptr) {
        printf("Encoder 创建外部 MPP buffer group 失败 ret=%d\n", ret);
        return false;
    }

    for (int i = 0; i < count; ++i) {
        EncoderExternalBuffer ext_buf;
        std::memset(&ext_buf, 0, sizeof(ext_buf));
        ext_buf.fd = -1;
        ext_buf.size = frame_size;

        if (dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, frame_size, &ext_buf.fd, &ext_buf.ptr) < 0) 
        {
            printf("Encoder DMA32 buffer 申请失败!\n");
            return false;
        }

        MppBufferInfo info;
        std::memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DMA_HEAP;
        info.size = frame_size;
        info.fd = ext_buf.fd;
        info.ptr = ext_buf.ptr;

        if (mpp_buffer_commit(buf_grp_, &info) != MPP_OK) 
        {
            printf("Encoder DMA32 buffer commit 到 MPP group 失败!\n");
            dma_buf_free(ext_buf.size, &ext_buf.fd, ext_buf.ptr);
            return false;
        }

        external_buffers_.push_back(ext_buf);
    }

    printf("Encoder 已分配并 commit %d 块 DMA32 输入 buffer。\n", count);
    return true;
}

void RkMppEncoder::ReleaseExternalBuffers()
{
    if (buf_grp_) {
        mpp_buffer_group_put(buf_grp_);
        buf_grp_ = nullptr;
    }

    for (auto& ext_buf : external_buffers_) {
        if (ext_buf.fd >= 0) {
            dma_buf_free(ext_buf.size, &ext_buf.fd, ext_buf.ptr);
            ext_buf.ptr = nullptr;
        }
    }
    external_buffers_.clear();
}

bool RkMppEncoder::GetHeader(std::vector<uint8_t>& header)
{
    header.clear();

    size_t size = 4096;
    header.resize(size);

    MppPacket packet = nullptr;
    MPP_RET ret = mpp_packet_init(&packet, header.data(), size);
    if (ret != MPP_OK || packet == nullptr) {
        header.clear();
        return false;
    }

    mpp_packet_set_length(packet, 0);

    ret = mpi_->control(ctx_, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret != MPP_OK) {
        mpp_packet_deinit(&packet);
        header.clear();
        return false;
    }

    size_t len = mpp_packet_get_length(packet);
    mpp_packet_deinit(&packet);

    if (len == 0 || len > size) {
        header.clear();
        return false;
    }

    header.resize(len);
    return true;
}
