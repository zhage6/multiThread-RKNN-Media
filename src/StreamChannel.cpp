#include "StreamChannel.h"
#include "TimingLogger.h"
#include "MultiModelPipeline.h"
#include "FfmpegH264Source.h"
#include "rkYolov5s.hpp"
#include <algorithm>
#include <chrono>

namespace 
{
    bool is_local_stream_url(const std::string& url)
    {
        return url.rfind("rtsp://", 0) != 0 &&
               url.rfind("rtmp://", 0) != 0 &&
               url.rfind("http://", 0) != 0 &&
               url.rfind("https://", 0) != 0;
    }

    bool is_network_stream_url(const std::string& url)
    {
        return !is_local_stream_url(url);
    }

    int64_t current_wall_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
} // namespace

VideoChannel::VideoChannel(int channel_id, const std::string& stream_url, 
                    MultiModelPipeline* pipeline, std::atomic<int>& active_cnt)
        : m_channel_id(channel_id), 
          m_stream_url(stream_url), 
          m_pipeline(pipeline),
          m_running(false),
          m_frame_counter(0),
          m_active_count(active_cnt),
          m_throttle_local_input(is_local_stream_url(stream_url)),
          m_input_fps(24),
          m_inflight_frames(0),
          m_max_inflight_frames(8)   //每路最大帧
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
}

void VideoChannel::start()
{
    if(m_running) return;
        m_running = true;
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

            data.width = w;
            data.height = h;
            data.hor_stride = h_stride;
            data.ver_stride = v_stride;
            // MppDecoder owns and releases MppFrame after this callback returns.
            // Async stages keep the image alive through data.src_buffer instead.
            data.channel_id = this->m_channel_id;    // 贴上通道标签
            data.frame_id = this->m_frame_counter++; // 贴上序号标签(满了怎么办？)
            data.pts_us = pts_us;                        // 当前阶段没有真实 PTS，先保留字段

            if (this->m_throttle_local_input && data.frame_id == 0)
            {
                // Start the local-file pacing clock from the first decoded
                // frame. Decoder initialization must not create a time debt
                // that makes the first frames run in a burst.
                this->m_input_start_time = std::chrono::steady_clock::now();
                timing::Log("local_input_clock_started ch=%d frame=%llu",
                            this->m_channel_id,
                            static_cast<unsigned long long>(data.frame_id));
            }

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
            const int max_inflight = m_max_inflight_frames;
            while (m_running)
            {
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

    if (m_decode_thread.joinable()) {
        m_decode_thread.join();
    }
}

void VideoChannel::DecodeLoop()
{
    if (is_network_stream_url(m_stream_url)) {
        DecodeNetworkInput();
    } else {
        DecodeFileInput();
    }
    printf("通道 %d 解码线程结束。\n", m_channel_id);
    m_active_count--;
}

bool VideoChannel::WaitForDecodeCapacity()
{
    while (m_running)
    {
        if (m_inflight_frames.load() < m_max_inflight_frames) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    while (m_running && m_pipeline && m_pipeline->PendingCount() >= 40)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return m_running;
}

void VideoChannel::DecodeFileInput()
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
        if (!WaitForDecodeCapacity()) {
            break;
        }
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
        if (bytes_read > 0) 
        {
            // 喂给当前通道的 MPP 解码器
            // 解出帧后由回调交给 MultiModelPipeline。
            m_decoder->DecodePacket(buffer, bytes_read);
        }
    }
    fclose(fp);
}

void VideoChannel::DecodeNetworkInput()
{
    while (m_running)
    {
        FfmpegH264Source source;
        if (!source.Open(m_stream_url)) {
            if (m_running) {
                printf("通道 %d 打开网络视频失败，1 秒后重试。\n", m_channel_id);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        while (m_running)
        {
            const FfmpegH264Source::ReadStatus status = source.ReadNext(
                [this](const uint8_t* data, size_t size, int64_t pts_us)
                {
                    if (!WaitForDecodeCapacity()) {
                        return false;
                    }
                    m_decoder->DecodePacket(data, size, pts_us);
                    return m_running.load();
                });

            if (status == FfmpegH264Source::ReadStatus::kPacket) {
                continue;
            }
            if (status == FfmpegH264Source::ReadStatus::kAgain) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (status == FfmpegH264Source::ReadStatus::kStopped || !m_running) {
                return;
            }

            printf("通道 %d 网络视频读取中断，重新连接。\n", m_channel_id);
            break;
        }

        source.Close();
        if (m_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
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
