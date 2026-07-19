#include "StreamChannel.h"
#include "TimingLogger.h"
#include "MultiModelPipeline.h"
#include <algorithm>
#include <chrono>

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

    int64_t current_wall_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
} // namespace

VideoChannel::VideoChannel(int channel_id, const std::string& stream_url, 
                    MultiModelPipeline* pipeline,std::atomic<int>& active_cnt, MosaicComposer* mosaic)
        : m_channel_id(channel_id), 
          m_stream_url(stream_url), 
          m_pipeline(pipeline),
          m_running(false),
          m_frame_counter(0),
          m_active_count(active_cnt),
          m_mosaic(mosaic),
          m_encoder(nullptr),
          m_encode_packet_counter(0),
          m_last_packet_pts(-1),
          m_output_fps(24),
          m_throttle_local_input(is_local_stream_url(stream_url)),
          m_input_fps(24),
          m_inflight_frames(0),
          m_max_inflight_frames(8),   //每路最大帧
          m_encoder_ready(false),
          m_startup_max_inflight_frames(2)
{
    // 实例化该通道专属的 MPP 解码器
    m_decoder = new MppDecoder();
}
VideoChannel::~VideoChannel() 
{
    Stop();
    if (m_decoder) 
    {
        delete m_decoder;
        m_decoder = nullptr;
    }
    if (m_encoder) 
    { 
        delete m_encoder; 
        m_encoder = nullptr; 
    }
    if (m_publisher) {
        m_publisher->Close();
    }
}

void VideoChannel::start()
{
    if(m_running) return;
        m_running = true;
        if (m_mosaic)
        {
            m_encoder_ready = true;
        }
        m_input_start_time = std::chrono::steady_clock::now();
        if (m_throttle_local_input) 
        {
            timing::Log("local_input_throttle_enabled ch=%d fps=%d url=%s",
                        m_channel_id,
                        m_input_fps,
                        m_stream_url.c_str());
        }
        m_decoder->Init([this](int src_fd, int w, int h, int h_stride, int v_stride, int64_t pts_us, MppFrame frame) 
        {
            // 当这个通道的 MPP 解出一帧时，会触发这里
            input_data data;
            data.src_fd = src_fd; 
            data.src_buffer = mpp_frame_get_buffer(frame);
            if (data.src_buffer) 
            {
                mpp_buffer_inc_ref(data.src_buffer); //后面的fd还需要继续的进行RGA，暂时不要释放
            }

            // if (m_encoder == nullptr) 
            // {
            //     InitEncoder(w, h, h_stride, v_stride, MPP_FMT_YUV420SP);
            // }
            data.width = w;
            data.height = h;
            data.hor_stride = h_stride;
            data.ver_stride = v_stride;
            // MppDecoder owns and releases MppFrame after this callback returns.
            // Async stages keep the image alive through data.src_buffer instead.
            data.frame = nullptr;
            data.channel_id = this->m_channel_id;    // 贴上通道标签
            data.frame_id = this->m_frame_counter++; // 贴上序号标签(满了怎么办？)
            data.pts_us = pts_us;                        // 当前阶段没有真实 PTS，先保留字段

            if (data.pts_us <= 0 && this->m_input_fps > 0) 
            {
                data.pts_us = static_cast<int64_t>(data.frame_id) * 1000000 / this->m_input_fps;
            }
            if (this->m_throttle_local_input && this->m_input_fps > 0) {
                auto target_time = this->m_input_start_time +
                    std::chrono::microseconds(data.frame_id * 1000000 / this->m_input_fps);

                while (this->m_running) 
                {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= target_time) 
                    {
                        break;
                    }

                    auto wake_time = std::min(target_time, now + std::chrono::milliseconds(5));
                    std::this_thread::sleep_until(wake_time);
                }
            }

            // 塞入全局共享的 RKNN 线程池！
            // 注意：如果池子满了，你的 m_pool->put 会阻塞，这天然形成了对当前解码线程的“反压”
            printf("一帧解码完成\n");
            int max_inflight = 0;
            while (m_running)
            {
                max_inflight = m_encoder_ready.load()
                    ? m_max_inflight_frames
                    : m_startup_max_inflight_frames;

                if (m_inflight_frames.load() < max_inflight) {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            if (!m_running) 
            {
                if (data.src_buffer) {
                    mpp_buffer_put(data.src_buffer);
                    data.src_buffer = nullptr;
                }
                return;
            }
            FrameContext task_frame;
            task_frame.channel_id = data.channel_id;
            task_frame.frame_id = data.frame_id;
            task_frame.pts_us = data.pts_us;
            task_frame.origin_wall_ms = current_wall_ms();
            task_frame.src_fd = data.src_fd;
            task_frame.src_buffer = data.src_buffer;
            task_frame.width = data.width;
            task_frame.height = data.height;
            task_frame.hor_stride = data.hor_stride;
            task_frame.ver_stride = data.ver_stride;

            int inflight_after = ++m_inflight_frames;
            bool submitted = this->m_pipeline && this->m_pipeline->Submit(task_frame);
            if (task_frame.src_buffer) 
            {
                mpp_buffer_put(task_frame.src_buffer);
                task_frame.src_buffer = nullptr;
            }
            if (!submitted) 
            {
                m_inflight_frames--;
                return;
            }

            timing::Log("decode_enqueue ch=%d frame=%llu pts_us=%lld inflight=%d max_inflight=%d queue=%zu",
                        this->m_channel_id,
                        static_cast<unsigned long long>(task_frame.frame_id),
                        static_cast<long long>(task_frame.pts_us),
                        inflight_after,
                        max_inflight,
                        this->m_pipeline->PendingCount());
        }, 
        MPP_VIDEO_CodingAVC    
        );
        m_decode_thread = std::thread(&VideoChannel::DecodeLoop, this);
}
void VideoChannel::Stop() 
{
    m_running = false;
    m_encoder_ready = false;

    if (m_decode_thread.joinable()) {
        m_decode_thread.join();
    }
}

void VideoChannel::DecodeLoop()
{
    FILE* fp = fopen(m_stream_url.c_str(), "rb");
    if (!fp) 
    {
        printf("通道 %d 打开视频失败！\n", m_channel_id);
        return;
    }
    unsigned char buffer[4096];
    while (m_running && !feof(fp)) 
    {
        while (m_running)
        {
            int max_inflight = m_encoder_ready.load()//如果编码器还没开始，即消费者还没启动，那么让该线程睡。少读取数据
                ? m_max_inflight_frames
                : m_startup_max_inflight_frames;

            if (m_inflight_frames.load() < max_inflight) 
            {
                break;
            }
            

            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        while (m_running && m_pipeline && m_pipeline->PendingCount() >= 40)  //临时控速 //这个是推理池子
        {
        // 如果池子满了，强行让当前读取线程睡 5 毫秒
        // 这样就不会继续往外吐 frame，MPP 解码器也就停下来了，内存涨幅瞬间停止！
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!m_running) 
        {
            break;
        }
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
        if (bytes_read > 0) 
        {
            // 喂给当前通道的 MPP 解码器
            // 它内部只要解出来，就会自动调用上面那个 Lambda 回调，扔进 testPool
            m_decoder->DecodePacket(buffer, bytes_read);
        }
    }
    fclose(fp);
    printf("通道 %d 解码线程结束。\n", m_channel_id);
    m_active_count--;
}
void VideoChannel::InitEncoder(int width, int height, int h_stride, int v_stride, MppFrameFormat fmt)
{
    m_encoder = new RkMppEncoder();
    m_encoder->Init(width, height, h_stride, v_stride, fmt, MPP_VIDEO_CodingAVC);
    m_stream_start_time = std::chrono::steady_clock::now();
    std::vector<uint8_t> h264_header;
    if (!m_encoder->GetHeader(h264_header)) 
    {
        printf("获取 H264 SPS/PPS 失败\n");
    }
   std::string rtsp_url =
    "rtsp://127.0.0.1:8554/live/channel" + std::to_string(m_channel_id);
    m_publisher.reset(new RtspPublisher());
    if (!m_publisher->Init(rtsp_url, width, height, m_output_fps, h264_header.data(), h264_header.size())) 
    {
        printf("通道 %d RTSP 推流初始化失败: %s\n", m_channel_id, rtsp_url.c_str());
    }
    m_encoder->SetOutputCallback([this](const uint8_t* data, size_t size, bool is_keyframe) 
    {
        printf("编码器编码完成一帧\n");
        auto callback_start = timing::Clock::now();
        const int fps = std::max(1, m_output_fps);
        const uint64_t frame_index = m_encode_packet_counter++;

        auto target_time = m_stream_start_time +
            std::chrono::microseconds(frame_index * 1000000 / fps);
        auto before_sleep = timing::Clock::now();
        long long wait_us = timing::UsBetween(before_sleep, target_time);
        std::this_thread::sleep_until(target_time);
        auto after_sleep = timing::Clock::now();
        long long late_us = timing::UsBetween(target_time, after_sleep);

        EncodedPacket packet;
        packet.channel_id = m_channel_id;
        packet.data = data;
        packet.size = size;
        packet.keyframe = is_keyframe;
        packet.pts = static_cast<int64_t>(frame_index * 90000 / fps);
        if (packet.pts <= m_last_packet_pts) {
            packet.pts = m_last_packet_pts + 1;
        }
        m_last_packet_pts = packet.pts;
        packet.dts = packet.pts;//时间戳
        auto push_start = timing::Clock::now();
        bool push_ok = false;
        if (m_publisher) {
            push_ok = m_publisher->Push(packet);
        }
        auto push_end = timing::Clock::now();
        timing::Log("packet_push ch=%d enc_frame=%llu size=%zu key=%d pts=%lld wait_us=%lld late_us=%lld push_us=%lld callback_us=%lld ok=%d",
                    m_channel_id,
                    static_cast<unsigned long long>(frame_index),
                    size,
                    is_keyframe ? 1 : 0,
                    static_cast<long long>(packet.pts),
                    wait_us > 0 ? wait_us : 0,
                    late_us > 0 ? late_us : 0,
                    timing::UsBetween(push_start, push_end),
                    timing::UsBetween(callback_start, push_end),
                    push_ok ? 1 : 0);
    });

    m_encoder->Start();
    m_encoder_ready = true;
    printf("\n通道 %d 的硬件编码器启动成功！\n", m_channel_id);
    
}

void VideoChannel::EncodeZeroCopy(const InferOutput& out) 
{
    IM_STATUS status;
    auto total_start = timing::Clock::now();
    if (out.src_buffer == nullptr || out.src_fd < 0) 
    {
        timing::Log("encode_input_drop ch=%d frame=%llu reason=bad_src",
                    m_channel_id,
                    static_cast<unsigned long long>(out.frame_id));
        release_source_buffer(out);
        return;
    }
    rga_buffer_t img = wrapbuffer_fd(out.src_fd, out.width, out.height,
                                 RK_FORMAT_YCbCr_420_SP,
                                 out.hor_stride, out.ver_stride);

    //利用RGA在结果上帮忙画图
    int drawn_boxes = 0;
    auto draw_start = timing::Clock::now();
    for (int i = 0; i < out.results.count; ++i) {
        const auto& res = out.results.results[i];
        int left = align_down_even(clamp_to_range(res.box.left, 0, out.width - 2));
        int top = align_down_even(clamp_to_range(res.box.top, 0, out.height - 2));
        int right = align_up_even(clamp_to_range(res.box.right, left + 2, out.width));
        int bottom = align_up_even(clamp_to_range(res.box.bottom, top + 2, out.height));
        int rect_w = right - left;
        int rect_h = bottom - top;
        if (rect_w < 2 || rect_h < 2) {
            continue;
        }

        im_rect rect = {left, top, rect_w, rect_h};
        status = imrectangle(img, rect, 0x0000ff00, 4);
        if (status != IM_STATUS_SUCCESS) {
            printf("RGA 画框失败: %s\n", imStrError(status));
        } else {
            drawn_boxes++;
        }
    }
    auto draw_end = timing::Clock::now();

    auto push_start = timing::Clock::now();
    if (!m_encoder->PushBuffer(out.src_buffer)) 
    {
        release_source_buffer(out);
        return;
    }
    auto push_end = timing::Clock::now();
    //release_source_buffer(out);

    timing::Log("encode_input ch=%d frame=%llu boxes=%d  draw_us=%lld enc_put_us=%lld total_us=%lld",
                m_channel_id,
                static_cast<unsigned long long>(out.frame_id),
                drawn_boxes,
                timing::UsBetween(draw_start, draw_end),
                timing::UsBetween(push_start, push_end),
                timing::UsBetween(total_start, timing::Clock::now()));
}

void VideoChannel::OnInferDropped()
{
    if (m_inflight_frames.load() > 0) {
        m_inflight_frames--;
    }
}


void VideoChannel::OnFrameAggregated(uint64_t frame_id)
{
    if (m_inflight_frames.load() > 0) 
    {
        m_inflight_frames--;
    }
}
