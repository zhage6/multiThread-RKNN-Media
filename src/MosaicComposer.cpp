#include "MosaicComposer.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include "TimingLogger.h"
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
namespace 
{
    void release_source_buffer(const InferOutput& out)
    {
        if (out.src_buffer) 
        {
            mpp_buffer_put(out.src_buffer);
        }
    }

    int clamp_to_range(int value, int low, int high)
    {
        return std::max(low, std::min(value, high));
    }

    int align_down_even(int value)
    {
        return value & ~1;
    }

    int align_up_even(int value)
    {
        return (value + 1) & ~1;
    }

    bool is_local_stream_url(const std::string& url)
    {
        return url.rfind("rtsp://", 0) != 0 &&
               url.rfind("rtmp://", 0) != 0 &&
               url.rfind("http://", 0) != 0 &&
               url.rfind("https://", 0) != 0;
    }
    struct RenderStats 
    {
        int boxes = 0;
        int unsupported = 0;
        int failed = 0;
    };

    const uint8_t kDigitFont[10][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
        {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
        {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    };

    const uint8_t kColonFont[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    const uint8_t kDotFont[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};

    void FillYRect(uint8_t* y_plane, int stride, int width, int height,
                   int x, int y, int w, int h, uint8_t value)
    {
        const int left = clamp_to_range(x, 0, width);
        const int top = clamp_to_range(y, 0, height);
        const int right = clamp_to_range(x + w, left, width);
        const int bottom = clamp_to_range(y + h, top, height);

        for (int row = top; row < bottom; ++row) {
            memset(y_plane + row * stride + left, value, right - left);
        }
    }

    void DrawGlyphY(uint8_t* y_plane, int stride, int width, int height,
                    int x, int y, const uint8_t glyph[7], int scale, uint8_t value)
    {
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] & (1 << (4 - col))) == 0) {
                    continue;
                }
                FillYRect(y_plane, stride, width, height,
                          x + col * scale,
                          y + row * scale,
                          scale,
                          scale,
                          value);
            }
        }
    }

    void DrawTextY(uint8_t* y_plane, int stride, int width, int height,
                   int x, int y, const char* text, int scale)
    {
        if (!y_plane || !text || scale <= 0) {
            return;
        }

        int len = static_cast<int>(strlen(text));
        int text_w = len * 6 * scale + 2 * scale;
        int text_h = 9 * scale;
        FillYRect(y_plane, stride, width, height,
                  x - scale,
                  y - scale,
                  text_w,
                  text_h,
                  32);

        int cursor = x;
        for (const char* p = text; *p; ++p) {
            if (*p >= '0' && *p <= '9') {
                DrawGlyphY(y_plane, stride, width, height,
                           cursor, y, kDigitFont[*p - '0'], scale, 235);
                cursor += 6 * scale;
            } else if (*p == ':') {
                DrawGlyphY(y_plane, stride, width, height,
                           cursor, y, kColonFont, scale, 235);
                cursor += 4 * scale;
            } else if (*p == '.') {
                DrawGlyphY(y_plane, stride, width, height,
                           cursor, y, kDotFont, scale, 235);
                cursor += 4 * scale;
            } else {
                cursor += 4 * scale;
            }
        }
    }

    void DrawWallClockOverlayNv12(void* ptr, int width, int height, int stride)
    {
        if (!ptr || width <= 0 || height <= 0 || stride <= 0) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        struct tm local_tm;
        localtime_r(&now_time, &local_tm);

        char text[32];
        snprintf(text, sizeof(text), "%02d:%02d:%02d.%03lld",
                 local_tm.tm_hour,
                 local_tm.tm_min,
                 local_tm.tm_sec,
                 static_cast<long long>(ms));

        DrawTextY(static_cast<uint8_t*>(ptr), stride, width, height, 24, 24, text, 4);
    }
} // namespace
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
    for (auto& cache : reusable_model_results_) {
        cache.clear();
    }
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
    for (auto& cache : reusable_model_results_) {
        cache.clear();
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

    memset(&input.results, 0, sizeof(input.results));
    input.model_results.clear();
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
    Submit(MakeYoloComposedFrame(out));
}

void MosaicComposer::Submit(const ComposedFrame& frame)
{
    const FrameContext& ctx = frame.frame;

    if (ctx.channel_id < 0 || ctx.channel_id >= kChannels) 
    {
        if (ctx.src_buffer) {
            mpp_buffer_put(ctx.src_buffer);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    submit_count_[ctx.channel_id]++;

    if (!initialized_) {
        if (ctx.src_buffer) {
            mpp_buffer_put(ctx.src_buffer);
        }
        return;
    }

    MosaicInput& slot = latest_[ctx.channel_id];

    if (slot.valid) {
        ReleaseInput(slot);
    }

    slot.valid = true;
    slot.channel_id = ctx.channel_id;
    slot.frame_id = ctx.frame_id;
    slot.pts_us = ctx.pts_us;
    slot.src_buffer = ctx.src_buffer;
    slot.src_fd = ctx.src_fd;
    slot.width = ctx.width;
    slot.height = ctx.height;
    slot.hor_stride = ctx.hor_stride;
    slot.ver_stride = ctx.ver_stride;
    slot.model_results = frame.results;

    for (const auto& result : frame.results) {
        if (result.ok && result.model_id == "face_yolo") {
            reusable_model_results_[ctx.channel_id][result.model_id] = result;
        }
    }

    bool has_face_result = false;
    for (const auto& result : slot.model_results) {
        if (result.model_id == "face_yolo") {
            has_face_result = true;
            break;
        }
    }

    if (!has_face_result) {
        auto cached = reusable_model_results_[ctx.channel_id].find("face_yolo");
        if (cached != reusable_model_results_[ctx.channel_id].end()) {
            slot.model_results.push_back(cached->second);
        }
    }

    bool has_face = false;
    int face_boxes = 0;
    int yolo_boxes = 0;
    for (const auto& result : frame.results) {
        if (result.model_id == "face_yolo") {
            has_face = true;
            face_boxes = result.detections.count;
        } else if (result.model_id == "yolo") {
            yolo_boxes = result.detections.count;
        }
    }
    timing::Log("mosaic_submit ch=%d frame=%llu partial=%d results=%zu has_face=%d face_boxes=%d yolo_boxes=%d",
                ctx.channel_id,
                static_cast<unsigned long long>(ctx.frame_id),
                frame.partial ? 1 : 0,
                frame.results.size(),
                has_face ? 1 : 0,
                face_boxes,
                yolo_boxes);

    // 先兼容 YOLO：从 composed results 里找 Detection 结果
    memset(&slot.results, 0, sizeof(slot.results));
    for (const auto& result : frame.results) {
        if (result.type == ModelResultType::Detection && result.ok) {
            slot.results = result.detections;
            break;
        }
    }

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
RenderStats RenderModelResult(const ModelResult& result,
                       const MosaicInput& input,
                       const im_rect& dst_rect,
                       rga_buffer_t dst_img,
                       int channel_index)
{
    RenderStats stats;
    if (!result.ok) 
    {
        stats.failed++;
        return stats;
    }

    switch (result.type) 
    {
    case ModelResultType::Detection:
    {
        const int box_color = (result.model_id == "face_yolo") ? 0x000000ff : 0x0000ff00;
        const float scale_x = static_cast<float>(dst_rect.width) / input.width;
        const float scale_y = static_cast<float>(dst_rect.height) / input.height;

        const int cell_left = dst_rect.x;
        const int cell_top = dst_rect.y;
        const int cell_right = dst_rect.x + dst_rect.width;
        const int cell_bottom = dst_rect.y + dst_rect.height;

        for (int j = 0; j < result.detections.count; ++j) 
        {
            const auto& res = result.detections.results[j];

            int src_left = clamp_to_range(res.box.left, 0, input.width - 2);
            int src_top = clamp_to_range(res.box.top, 0, input.height - 2);
            int src_right = clamp_to_range(res.box.right, src_left + 2, input.width);
            int src_bottom = clamp_to_range(res.box.bottom, src_top + 2, input.height);

            int left = cell_left + static_cast<int>(src_left * scale_x);
            int top = cell_top + static_cast<int>(src_top * scale_y);
            int right = cell_left + static_cast<int>(src_right * scale_x);
            int bottom = cell_top + static_cast<int>(src_bottom * scale_y);

            left = align_down_even(clamp_to_range(left, cell_left, cell_right - 2));
            top = align_down_even(clamp_to_range(top, cell_top, cell_bottom - 2));
            right = align_up_even(clamp_to_range(right, left + 2, cell_right));
            bottom = align_up_even(clamp_to_range(bottom, top + 2, cell_bottom));

            im_rect rect;
            rect.x = left;
            rect.y = top;
            rect.width = right - left;
            rect.height = bottom - top;

            IM_STATUS rect_status = imrectangle(dst_img, rect, box_color, 4);
            if (rect_status != IM_STATUS_SUCCESS) 
            {
                printf("Mosaic draw box failed ch=%d box=%d status=%d\n",
                       channel_index, j, rect_status);
            }

            if (res.has_landmarks) {
                for (int k = 0; k < FACE_LANDMARK_NUM; ++k) {
                    int src_x = clamp_to_range(res.landmarks[k][0], 0, input.width - 1);
                    int src_y = clamp_to_range(res.landmarks[k][1], 0, input.height - 1);

                    int center_x = cell_left + static_cast<int>(src_x * scale_x);
                    int center_y = cell_top + static_cast<int>(src_y * scale_y);

                    im_rect point;
                    point.x = align_down_even(clamp_to_range(center_x - 2, cell_left, cell_right - 4));
                    point.y = align_down_even(clamp_to_range(center_y - 2, cell_top, cell_bottom - 4));
                    point.width = 4;
                    point.height = 4;

                    IM_STATUS point_status = imfill(dst_img, point, 0x00ff0000);
                    if (point_status != IM_STATUS_SUCCESS) {
                        printf("Mosaic draw landmark failed ch=%d box=%d point=%d status=%d\n",
                               channel_index, j, k, point_status);
                    }
                }
            }
        }
        stats.boxes++;
        if (result.model_id == "face_yolo" && result.detections.count > 0) 
        {
            timing::Log("mosaic_render_face ch=%d frame=%llu boxes=%d",
                        channel_index,
                        static_cast<unsigned long long>(input.frame_id),
                        result.detections.count);
        }
        break;
    }

    case ModelResultType::Classification:
    case ModelResultType::Segmentation:
    case ModelResultType::Keypoints:
    case ModelResultType::Custom:
    default:
        // 第一阶段先不渲染这些类型，避免阻塞多模型框架接入。
        stats.unsupported++;
        break;
    }
    return stats;
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
        
        if (status != IM_STATUS_SUCCESS) 
        {
            printf("Mosaic RGA failed ch=%d status=%d\n", i, status);
            mosaic_rga_fail_count_++;
        }
        if (status == IM_STATUS_SUCCESS) 
        {
            if (!input.model_results.empty()) 
            {
                for (const auto& result : input.model_results) 
                {
                    RenderModelResult(result, input, dst_rect, dst_img, i);
                }
            } 
            else 
            {
                // 兼容旧路径：如果还没有 model_results，就用旧 results 画 detection。
                ModelResult legacy;
                legacy.model_id = "legacy-yolo";
                legacy.type = ModelResultType::Detection;
                legacy.ok = true;
                legacy.detections = input.results;

                RenderModelResult(legacy, input, dst_rect, dst_img, i);
            }
        }
    }
    mosaic_compose_count_++;

    void* dst_ptr = dst_info.ptr ? dst_info.ptr : mpp_buffer_get_ptr(dst_buffer);
    DrawWallClockOverlayNv12(dst_ptr, out_width_, out_height_, out_hor_stride_);

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
