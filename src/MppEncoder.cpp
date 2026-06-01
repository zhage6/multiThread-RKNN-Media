#include "MppEncoder.h"
#include <rockchip/mpp_debug.h>
RkMppEncoder::RkMppEncoder() 
    : ctx_(nullptr), 
      mpi_(nullptr), 
      cfg_(nullptr), 
      buf_grp_(nullptr), 
      is_running_(false),
      width_(0),
      height_(0),
      fmt_(MPP_FMT_YUV420SP) 
      {}

RkMppEncoder::~RkMppEncoder() 
{
    Stop();
}
bool RkMppEncoder::Init(int width, int height, MppFrameFormat fmt, MppCodingType type)
{
    width_ = width;
    height_ = height;
    fmt_ = fmt;
    MPP_RET ret = MPP_OK;
    mpp_create(&ctx_, &mpi_);
    mpp_init(ctx_, MPP_CTX_ENC, type);

    // 2. 初始化 cfg 对象并获取硬件默认值
    mpp_enc_cfg_init(&cfg_);
    mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_);

    // 3. 手动设置你需要的编码参数 (字典式键值对设置)
    
    // --- 准备阶段配置 (输入图像信息) ---
    mpp_enc_cfg_set_s32(cfg_, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg_, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", width); // 通常水平步长等于宽度，若是按16或64对齐请按需修改
    mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(cfg_, "prep:format", fmt);

    // --- 码率控制配置 (RC: Rate Control) ---
    int fps_in = 30;
    int fps_out = 30;
    int bps = width * height * fps_out / 14; // 这是一个粗略的码率估算公式，可自定义
    
    // 设置 CBR (固定码率) 或 VBR (动态码率)
    mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR); 
    
    // 设置帧率
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps_in);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_den", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps_out);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_den", 1);
    
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
    if (ret != MPP_OK) {
        // 处理配置下发失败的情况
        return false;
    }
    mpp_buffer_group_get_internal(&buf_grp_, MPP_BUFFER_TYPE_DRM); //暂定内部分配

    //size_t frame_size = width * height * 1.5; // 以 YUV420SP (NV12) 为例
    // for (int i = 0; i < 4; i++) 
    // {
    //     MppBuffer buffer = nullptr;
    //     mpp_buffer_get(buf_grp_, &buffer, frame_size);
    //     free_buffers_.push(buffer); // 放入空闲队列
    // }
    size_t frame_size = width_ * height_ * 1.5; // NV12 格式大小
    mpp_buffer_group_limit_config(buf_grp_, frame_size, 4);
    printf("Encoder Init Success. Width: %d, Height: %d", width_, height_);
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

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    
    // 设置常规参数
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, fmt_); // 如 MPP_FMT_YUV420SP
    
    // 绑定已经带有画框数据的物理内存
    mpp_frame_set_buffer(frame, buffer);

    MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
    if (ret != MPP_OK) {
        printf("送入编码器失败!\n");
    }

    // 销毁 Frame 外壳
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buffer);
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
            void* data = mpp_packet_get_pos(packet);
            size_t size = mpp_packet_get_length(packet);
            
            // 判断是否是 I 帧
            uint32_t flags = mpp_packet_get_flag(packet);
            printf("\n>>> [输出线程] 收到硬件码流包! 长度: %zu 字节, flags: 0x%08x\n", size, flags);
            bool is_keyframe = (flags & MPP_PACKET_FLAG_INTRA) ? true : false;
            
           if (size > 0 && on_packet_ready_) 
           {
                on_packet_ready_((const uint8_t*)data, size, is_keyframe);
           }

            // // 2. 内存回收闭环：找出产生这个包的原始输入帧
            // MppMeta meta = mpp_packet_get_meta(packet);
            // MppFrame orig_frame = nullptr;
            
            // if (MPP_OK == mpp_meta_get_frame(meta, KEY_INPUT_FRAME, &orig_frame)) 
            // {
            //     mpp_assert(orig_frame);
            //     MppBuffer used_buffer = mpp_frame_get_buffer(orig_frame);
            //     printf("一帧码流准备好了，正在回收物理 Buffer...\n");
            //     // 放回空闲池并唤醒阻塞的 PushFrame
            //     {
            //         std::lock_guard<std::mutex> lock(mtx_);
            //         free_buffers_.push(used_buffer);
            //     }
            //     cv_.notify_one(); 
            //     printf("一帧编码完成，已回收物理 Buffer,唤醒等待的线程\n");
            //     mpp_frame_deinit(&orig_frame);
            // }
            // printf("警告：无法找到原始输入帧，可能导致内存泄漏！\n");
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

void RkMppEncoder::Stop() 
{
    if (!is_running_) return;
    
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

    // 4. 清理空闲的物理 Buffer
    {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!free_buffers_.empty()) {
            MppBuffer buf = free_buffers_.front();
            mpp_buffer_put(buf); // 释放物理内存
            free_buffers_.pop();
        }
    }

    // 5. 销毁 MPP 各种句柄
    if (buf_grp_) {
        mpp_buffer_group_put(buf_grp_);
        buf_grp_ = nullptr;
    }

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
    // std::unique_lock<std::mutex> lock(mtx_);
    // // 阻塞等待直到有空闲的 Buffer，或者编码器停止
    // cv_.wait(lock, [this]() { return !free_buffers_.empty() || !is_running_; });
    
    // if (!is_running_ || free_buffers_.empty()) {
    //     return nullptr;
    // }
    
    // MppBuffer buf = free_buffers_.front();
    // free_buffers_.pop();
    // return buf;
    MppBuffer buffer = nullptr;
    size_t frame_size = width_ * height_ * 1.5; 

    while (is_running_) 
    {
        // 尝试从硬件池中获取内存（拿出来时引用计数自动 +1）
        MPP_RET ret = mpp_buffer_get(buf_grp_, &buffer, frame_size);
        
        if (ret == MPP_OK && buffer != nullptr) {
            return buffer; // 成功拿到空闲内存，返回给上层去填充图像数据
        }
        
        // 如果 4 块内存都在被硬件使用，get 会失败。稍微等 2 毫秒再试。
        // 这就代替了你原来的 cv_.wait()，而且绝对不会死锁！
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return nullptr;
}