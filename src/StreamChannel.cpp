#include "StreamChannel.h"

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
        InitEncoder(out.image.cols, out.image.rows, MPP_FMT_YUV420SP); // 根据实际格式调整
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
    // 【做法 A：完美零拷贝】
    // 如果你的 NPU 后处理（画框）是直接通过 RGA 画在了原始解码的 MppBuffer 上
    // 那么 out 结构体里应该携带那个 mpp_buffer，你只需直接 Push：
    // m_encoder->PushBuffer(out.mpp_buffer);

    // 【做法 B：半零拷贝 (Mat 转 Buffer)】
    // 如果 out.image 是一个 cv::Mat (CPU内存)，你需要向 Encoder 借一块物理内存，拷进去再送给硬件
    MppBuffer mpp_buf = m_encoder->GetFreeBuffer();
    if (mpp_buf != nullptr) {
        void* ptr = mpp_buffer_get_ptr(mpp_buf);
        size_t size = mpp_buffer_get_size(mpp_buf);
        
        // 注意：这里需要确保 cv::Mat 的数据格式与编码器期望的格式 (如 YUV420SP) 一致！
        // 如果 cv::Mat 是 BGR，你可能还需要调用 RGA 把 BGR 转换成 NV12 并直接写入 mpp_buf
        memcpy(ptr, out.image.data, std::min(size, (size_t)(out.image.total() * out.image.elemSize())));
        
        m_encoder->PushBuffer(mpp_buf);
    }
}

