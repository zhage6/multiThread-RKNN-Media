#include "MosaicComposer.h"
#include <cstdio>
#include <cstring>
#include "TimingLogger.h"
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
MosaicComposer::MosaicComposer()
    : out_width_(0),
      out_height_(0),
      fps_(24),
      initialized_(false),
      stats_last_(std::chrono::steady_clock::now())
{
}

MosaicComposer::~MosaicComposer()
{
    Stop();
}



bool MosaicComposer::Init(int out_width, int out_height, int fps)
{
    std::lock_guard<std::mutex> lock(mtx_);
    out_width_ = out_width;
    out_height_ = out_height;
    fps_ = fps;
    initialized_ = true;
    out_hor_stride_ = MPP_ALIGN(out_width_, 16);
    out_ver_stride_ = MPP_ALIGN(out_height_, 16);
    out_buf_size_ = out_hor_stride_ * out_ver_stride_ * 3 / 2;
    if (!AllocateOutputBuffers(4)) 
    {
        initialized_ = false;
        return false;
    }

    encoder_ = std::make_unique<RkMppEncoder>();

    if (!encoder_->Init(out_width_,
                        out_height_,
                        out_hor_stride_,
                        out_ver_stride_,
                        MPP_FMT_YUV420SP,
                        MPP_VIDEO_CodingAVC)) 
    {
        printf("Mosaic encoder init failed\n");
        ReleaseOutputBuffers();
        initialized_ = false;
        return false;
    }
    std::vector<uint8_t> h264_header;
    if (!encoder_->GetHeader(h264_header)) {
        printf("Mosaic encoder get header failed\n");
    }

    publisher_ = std::make_unique<RtspPublisher>();
    if (!publisher_->Init("rtsp://127.0.0.1:8554/live/mosaic",
                        out_width_,
                        out_height_,
                        fps_,
                        h264_header.data(),
                        h264_header.size())) {
        printf("Mosaic RTSP publisher init failed\n");
        encoder_->Stop();
        encoder_.reset();
        ReleaseOutputBuffers();
        initialized_ = false;
        return false;
    }

    packet_counter_ = 0;
    last_packet_pts_ = -1;
    mosaic_push_count_ = 0;
    mosaic_tick_count_ = 0;
    mosaic_not_ready_count_ = 0;
    mosaic_compose_count_ = 0;
    mosaic_busy_drop_count_ = 0;
    mosaic_rga_fail_count_ = 0;
    stats_last_submit_total_ = 0;
    stats_last_tick_count_ = 0;
    stats_last_not_ready_count_ = 0;
    stats_last_compose_count_ = 0;
    stats_last_push_count_ = 0;
    stats_last_busy_drop_count_ = 0;
    submit_count_.fill(0);
    stream_start_time_ = std::chrono::steady_clock::now();
    stats_last_ = stream_start_time_;

    encoder_->SetOutputCallback([this](const uint8_t* data, size_t size, bool is_keyframe) {
        if (!publisher_ || data == nullptr || size == 0) 
        {
            return;
        }
        const int fps = std::max(1, fps_);
        const uint64_t frame_index = packet_counter_++;

        EncodedPacket packet;
        packet.channel_id = 0;
        packet.data = data;
        packet.size = size;
        packet.keyframe = is_keyframe;
        packet.pts = static_cast<int64_t>(frame_index * 90000 / fps);

        if (packet.pts <= last_packet_pts_) {
            packet.pts = last_packet_pts_ + 1;
        }

        last_packet_pts_ = packet.pts;
        packet.dts = packet.pts;

        publisher_->Push(packet);
    });

    if (!encoder_->Start()) {
        printf("Mosaic encoder start failed\n");
        publisher_->Close();
        publisher_.reset();
        encoder_->Stop();
        encoder_.reset();
        ReleaseOutputBuffers();
        initialized_ = false;
        return false;
    }

    flow_running_ = true;
    flow_thread_ = std::thread(&MosaicComposer::FlowLoop, this);

    return true;
}

bool MosaicComposer::AllocateOutputBuffers(int count)
{
    MPP_RET ret = mpp_buffer_group_get_external(&mosaic_grp_, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret != MPP_OK || mosaic_grp_ == nullptr) {
        printf("Mosaic mpp_buffer_group_get_external failed ret=%d\n", ret);
        return false;
    }

    for (int i = 0; i < count; ++i) {
        MosaicDmaBuffer buf;
        buf.size = out_buf_size_;

        if (dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH,
                          buf.size,
                          &buf.fd,
                          &buf.ptr) < 0) {
            printf("Mosaic DMA buffer alloc failed\n");
            return false;
        }

        MppBufferInfo info;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_DMA_HEAP;
        info.size = buf.size;
        info.fd = buf.fd;
        info.ptr = buf.ptr;

        ret = mpp_buffer_commit(mosaic_grp_, &info);
        if (ret != MPP_OK) {
            printf("Mosaic mpp_buffer_commit failed ret=%d\n", ret);
            dma_buf_free(buf.size, &buf.fd, buf.ptr);
            return false;
        }

        output_buffers_.push_back(buf);
    }

    return true;
}

void MosaicComposer::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        initialized_ = false;
        flow_running_ = false;
    }

    flow_cv_.notify_all();
    if (flow_thread_.joinable()) {
        flow_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mtx_);

    for (auto& input : latest_) {
        ReleaseInput(input);
    }

    if (encoder_) {
        encoder_->Stop();
        encoder_.reset();
    }

    if (publisher_) {
        publisher_->Close();
        publisher_.reset();
    }

    ReleaseOutputBuffers();
}

void MosaicComposer::ReleaseOutputBuffers()
{
    if (mosaic_grp_) {
        mpp_buffer_group_put(mosaic_grp_);
        mosaic_grp_ = nullptr;
    }

    for (auto& buf : output_buffers_) {
        if (buf.fd >= 0) {
            dma_buf_free(buf.size, &buf.fd, buf.ptr);
            buf.fd = -1;
            buf.ptr = nullptr;
        }
    }

    output_buffers_.clear();
}


void MosaicComposer::ReleaseInput(MosaicInput& input)
{
    if (input.src_buffer) 
    {
        mpp_buffer_put(input.src_buffer);
        input.src_buffer = nullptr;
    }

    input.src_fd = -1;
    input.channel_id = -1;
    input.frame_id = 0;
    input.pts_us = -1;
    input.valid = false;
}

bool MosaicComposer::ReadyLocked() const
{
    for (const auto& input : latest_) 
    {
        if (!input.valid) {
            return false;
        }
    }
    return true;
}

void MosaicComposer::Submit(const InferOutput& out)
{
    if (out.channel_id < 0 || out.channel_id >= kChannels) 
    {
        if (out.src_buffer) 
        {
            mpp_buffer_put(out.src_buffer);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    submit_count_[out.channel_id]++;

    if (!initialized_) {
        if (out.src_buffer) {
            mpp_buffer_put(out.src_buffer);
        }
        return;
    }

    MosaicInput& slot = latest_[out.channel_id];

    // 新帧覆盖本通道 latest 时释放旧帧，避免 decoder buffer 泄漏。
    if (slot.valid) 
    {
        ReleaseInput(slot);
    }

    slot.valid = true;
    slot.channel_id = out.channel_id;
    slot.frame_id = out.frame_id;
    slot.pts_us = out.pts_us;
    slot.src_buffer = out.src_buffer;
    slot.src_fd = out.src_fd;
    slot.width = out.width;
    slot.height = out.height;
    slot.hor_stride = out.hor_stride;
    slot.ver_stride = out.ver_stride;
    slot.results = out.results;

    MaybeLogStatsLocked("submit");
}

void MosaicComposer::FlowLoop()
{
    const int fps = std::max(1, fps_);
    const auto period = std::chrono::microseconds(1000000 / fps);
    auto next_tick = std::chrono::steady_clock::now() + period;

    while (true) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (flow_cv_.wait_until(lock, next_tick, [this]() { return !flow_running_; })) {
            break;
        }

        if (!flow_running_) {
            break;
        }

        mosaic_tick_count_++;
        if (initialized_ && ReadyLocked()) {
            ComposeLocked();
        } else {
            mosaic_not_ready_count_++;
            MaybeLogStatsLocked("mosaic_not_ready");
        }

        auto now = std::chrono::steady_clock::now();
        next_tick += period;
        if (next_tick <= now) {
            next_tick = now + period;
        }
    }
}

void MosaicComposer::ComposeLocked()
{
    if (!ReadyLocked()) {
        mosaic_not_ready_count_++;
        MaybeLogStatsLocked("mosaic_not_ready");
        return;
    }

    MppBuffer dst_buffer = nullptr;
    MPP_RET ret = mpp_buffer_get(mosaic_grp_, &dst_buffer, out_buf_size_);

    if (ret != MPP_OK || dst_buffer == nullptr) 
    {
        printf("Mosaic output buffer busy, drop one mosaic frame ret=%d\n", ret);
        mosaic_busy_drop_count_++;
        MaybeLogStatsLocked("mosaic_busy");
        return;
    }

    MppBufferInfo dst_info;
    memset(&dst_info, 0, sizeof(dst_info));
    if (mpp_buffer_info_get(dst_buffer, &dst_info) != MPP_OK || dst_info.fd < 0) 
    {
        printf("Mosaic mpp_buffer_info_get failed\n");
        mpp_buffer_put(dst_buffer);
        mosaic_busy_drop_count_++;
        MaybeLogStatsLocked("mosaic_bad_output_buffer");
        return;
    }
    int dst_fd = dst_info.fd;



    const int cell_w = out_width_ / 2;
    const int cell_h = out_height_ / 2;

    for (int i = 0; i < kChannels; ++i) 
    {
        auto& input = latest_[i];

        rga_buffer_t src = wrapbuffer_fd(input.src_fd,
                                 input.width,
                                 input.height,
                                 RK_FORMAT_YCbCr_420_SP,
                                 input.hor_stride,
                                 input.ver_stride);


        rga_buffer_t dst_img = wrapbuffer_fd(dst_fd,
                                     out_width_,
                                     out_height_,
                                     RK_FORMAT_YCbCr_420_SP,
                                     out_hor_stride_,
                                     out_ver_stride_);

        im_rect src_rect;
        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.width = input.width;
        src_rect.height = input.height;

        im_rect dst_rect;
        dst_rect.x = (i % 2) * cell_w;
        dst_rect.y = (i / 2) * cell_h;
        dst_rect.width = cell_w;
        dst_rect.height = cell_h;

        im_rect empty_rect;
        memset(&empty_rect, 0, sizeof(empty_rect));

        rga_buffer_t pat;
        memset(&pat, 0, sizeof(pat));

        IM_STATUS status = improcess(src,
                             dst_img,
                             pat,
                             src_rect,
                             dst_rect,
                             empty_rect,
                             IM_SYNC);

        if (status != IM_STATUS_SUCCESS) {
            printf("Mosaic RGA failed ch=%d status=%d\n", i, status);
            mosaic_rga_fail_count_++;
        }

    }
    mosaic_compose_count_++;

    printf("Mosaic compose success: ch0=%llu ch1=%llu ch2=%llu ch3=%llu\n",
           static_cast<unsigned long long>(latest_[0].frame_id),
           static_cast<unsigned long long>(latest_[1].frame_id),
           static_cast<unsigned long long>(latest_[2].frame_id),
           static_cast<unsigned long long>(latest_[3].frame_id));

   if (!encoder_ || !encoder_->PushBuffer(dst_buffer)) 
    {
        printf("Mosaic encoder push failed\n");
        mpp_buffer_put(dst_buffer);
    }
    else
    {
        mosaic_push_count_++;
        printf("Mosaic pushed=%llu duration@%dfps=%.2fs\n",
            static_cast<unsigned long long>(mosaic_push_count_),
            fps_,
            mosaic_push_count_ * 1.0 / fps_);
    }
    MaybeLogStatsLocked("compose");
}

void MosaicComposer::MaybeLogStatsLocked(const char* source)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats_last_).count();
    if (elapsed_ms < 1000) {
        return;
    }

    uint64_t submit_total = 0;
    for (uint64_t count : submit_count_) {
        submit_total += count;
    }

    double seconds = elapsed_ms / 1000.0;
    timing::Log("mosaic_health source=%s submit_fps=%.2f tick_fps=%.2f not_ready_fps=%.2f compose_fps=%.2f push_fps=%.2f busy_drop_fps=%.2f total_submit=%llu total_tick=%llu total_not_ready=%llu total_compose=%llu total_push=%llu rga_fail=%llu latest_frame=%llu,%llu,%llu,%llu latest_pts_us=%lld,%lld,%lld,%lld",
                source ? source : "unknown",
                (submit_total - stats_last_submit_total_) / seconds,
                (mosaic_tick_count_ - stats_last_tick_count_) / seconds,
                (mosaic_not_ready_count_ - stats_last_not_ready_count_) / seconds,
                (mosaic_compose_count_ - stats_last_compose_count_) / seconds,
                (mosaic_push_count_ - stats_last_push_count_) / seconds,
                (mosaic_busy_drop_count_ - stats_last_busy_drop_count_) / seconds,
                static_cast<unsigned long long>(submit_total),
                static_cast<unsigned long long>(mosaic_tick_count_),
                static_cast<unsigned long long>(mosaic_not_ready_count_),
                static_cast<unsigned long long>(mosaic_compose_count_),
                static_cast<unsigned long long>(mosaic_push_count_),
                static_cast<unsigned long long>(mosaic_rga_fail_count_),
                static_cast<unsigned long long>(latest_[0].frame_id),
                static_cast<unsigned long long>(latest_[1].frame_id),
                static_cast<unsigned long long>(latest_[2].frame_id),
                static_cast<unsigned long long>(latest_[3].frame_id),
                static_cast<long long>(latest_[0].pts_us),
                static_cast<long long>(latest_[1].pts_us),
                static_cast<long long>(latest_[2].pts_us),
                static_cast<long long>(latest_[3].pts_us));

    stats_last_ = now;
    stats_last_submit_total_ = submit_total;
    stats_last_tick_count_ = mosaic_tick_count_;
    stats_last_not_ready_count_ = mosaic_not_ready_count_;
    stats_last_compose_count_ = mosaic_compose_count_;
    stats_last_push_count_ = mosaic_push_count_;
    stats_last_busy_drop_count_ = mosaic_busy_drop_count_;
}
