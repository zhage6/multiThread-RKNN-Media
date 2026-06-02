#include "StreamChannel.h"
#include <algorithm>

namespace {

void release_source_buffer(const InferOutput& out)
{
    if (out.src_buffer) {
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

} // namespace

VideoChannel::VideoChannel(int channel_id, const std::string& stream_url, 
                    GlobalPool* pool,std::atomic<int>& active_cnt)
        : m_channel_id(channel_id), 
          m_stream_url(stream_url), 
          m_pool(pool),
          m_running(false),
          m_frame_counter(0),
          m_active_count(active_cnt),
          m_expected_frame_id(0), // 初始化期待的帧号
          m_encoder(nullptr),
          m_out_fp(nullptr)
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
    if (m_out_fp)  
    {   
        fclose(m_out_fp); 
        m_out_fp = nullptr; 
    }
}

void VideoChannel::start()
{
    if(m_running) return;
        m_running = true;
        m_decoder->Init([this](int src_fd, int w, int h, int h_stride, int v_stride, MppFrame frame) {
            // 当这个通道的 MPP 解出一帧时，会触发这里
            input_data data;
            data.src_fd = src_fd; 
            data.src_buffer = mpp_frame_get_buffer(frame);
            if (data.src_buffer) {
                mpp_buffer_inc_ref(data.src_buffer);
            }
            data.width = w;
            data.height = h;
            data.hor_stride = h_stride;
            data.ver_stride = v_stride;
            data.frame = frame;
            data.channel_id = this->m_channel_id;    // 贴上通道标签
            data.frame_id = this->m_frame_counter++; // 贴上序号标签(满了怎么办？)
            // 塞入全局共享的 RKNN 线程池！
            // 注意：如果池子满了，你的 m_pool->put 会阻塞，这天然形成了对当前解码线程的“反压”
            printf("一帧解码完成\n");
            this->m_pool->put(data); 
        }, 
        MPP_VIDEO_CodingAVC    
        );
        m_decode_thread = std::thread(&VideoChannel::DecodeLoop, this);
}
void VideoChannel::Stop() 
{
    m_running = false;
    if (m_decode_thread.joinable()) m_decode_thread.join();
}
void VideoChannel::OnInferOutput(const InferOutput& out) 
{
    std::lock_guard<std::mutex> lock(m_reorder_mtx);

    // 1. 延迟初始化编码器 (在这里我们才能确定图像真正的宽高)
    if (m_encoder == nullptr) 
    {
        InitEncoder(out.width, out.height, MPP_FMT_YUV420SP);
    }

    // 2. 放入重排缓冲区
    m_reorder_buffer[out.frame_id] = out;

    // 3. 顺序消费逻辑
    while (m_reorder_buffer.count(m_expected_frame_id) > 0) 
    {
        auto current_out = m_reorder_buffer[m_expected_frame_id];
        m_reorder_buffer.erase(m_expected_frame_id);

        // ---> 核心：零拷贝推流到编码器 <---
        EncodeZeroCopy(current_out);

        m_expected_frame_id++;
    }

    // 4. 防爆内存保护 (防丢帧卡死)
    if (m_reorder_buffer.size() > 20) {
        printf("警告：通道 %d 出现严重丢帧！强制向前推进序号。\n", m_channel_id);
        m_expected_frame_id = m_reorder_buffer.begin()->first;
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
        while (m_pool->get_task_size() >= 20) //临时控速
        {
        // 如果池子满了，强行让当前读取线程睡 5 毫秒
        // 这样就不会继续往外吐 frame，MPP 解码器也就停下来了，内存涨幅瞬间停止！
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
void VideoChannel::InitEncoder(int width,int height,MppFrameFormat fmt)
{
    m_encoder = new RkMppEncoder();
    m_encoder->Init(width, height, fmt, MPP_VIDEO_CodingAVC);
    std::string out_file = "result_channel_" + std::to_string(m_channel_id) + ".h264";
    m_out_fp = fopen(out_file.c_str(), "wb");
    m_encoder->SetOutputCallback([this](const uint8_t* data, size_t size, bool is_keyframe) 
    {
        printf("编码器编码完成一帧\n");
        if (m_out_fp) 
        {
            fwrite(data, 1, size, m_out_fp);
        }
    });

    m_encoder->Start();
    printf("\n通道 %d 的硬件编码器启动成功！\n", m_channel_id);
    
}

void VideoChannel::EncodeZeroCopy(const InferOutput& out) 
{
    if (out.src_buffer == nullptr || out.src_fd < 0) {
        return;
    }

    MppBuffer mpp_buf = m_encoder->GetFreeBuffer();
    if (mpp_buf == nullptr) {
        release_source_buffer(out);
        return;
    }

    int dst_fd = mpp_buffer_get_fd(mpp_buf);
    rga_buffer_t src_img = wrapbuffer_fd(out.src_fd, out.width, out.height,
                                         RK_FORMAT_YCbCr_420_SP,
                                         out.hor_stride, out.ver_stride);
    rga_buffer_t dst_img = wrapbuffer_fd(dst_fd, out.width, out.height,
                                         RK_FORMAT_YCbCr_420_SP,
                                         out.width, out.height);

    IM_STATUS status = imcopy(src_img, dst_img);
    if (status != IM_STATUS_SUCCESS) {
        printf("RGA 编码前拷贝失败: %s\n", imStrError(status));
        mpp_buffer_put(mpp_buf);
        release_source_buffer(out);
        return;
    }

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
        status = imrectangle(dst_img, rect, 0x0000ff00, 4);
        if (status != IM_STATUS_SUCCESS) {
            printf("RGA 画框失败: %s\n", imStrError(status));
        }
    }

    m_encoder->PushBuffer(mpp_buf);
    release_source_buffer(out);
}
