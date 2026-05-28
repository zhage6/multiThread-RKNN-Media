#include <thread>
#include <string>
#include <atomic>
#include <iostream>
#include "MppDecoder.h" // 你现有的 MPP 解码器
#include "rknnPool.hpp"
#include "rkYolov5s.hpp"


struct DetectResult {
    int class_id;
    float score;
    int x1, y1, x2, y2;
};

// 在流水线中传递的核心任务包
struct MediaTask {
    int channel_id;          // 通道号 (例如 0, 1, 2, 3)
    uint64_t frame_id;       // 帧序号 (用于后续重排)
    
    int dma_fd;              // MPP解码输出的物理内存句柄 (零拷贝核心)
    int width;               // 图像宽
    int height;              // 图像高
    int format;              // 图像格式 (如 RK_FORMAT_YCbCr_420_SP)
    
    std::vector<DetectResult> ai_results; // 用于存放稍后NPU推理的结果
};

using MediaTaskPtr = std::shared_ptr<MediaTask>;
using GlobalPool = rknnPool<rkYolov5s, input_data, InferOutput>;
class VideoChannel
{
    public:
        VideoChannel(int channel_id, const std::string& stream_url, 
                    GlobalPool* pool,std::atomic<int>& active_cnt)
        : m_channel_id(channel_id), 
          m_stream_url(stream_url), 
          m_pool(pool),
          m_running(false),
          m_frame_counter(0),
          m_active_count(active_cnt)
        {
            // 实例化该通道专属的 MPP 解码器
            m_decoder = new MppDecoder();
        }
        ~VideoChannel() 
        {
            Stop();
            if (m_decoder) 
            {
                delete m_decoder;
                m_decoder = nullptr;
            }
        }
        void start()
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
        void Stop() 
        {
            m_running = false;
            if (m_decode_thread.joinable()) m_decode_thread.join();
        }
    private: 
        void DecodeLoop() 
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
        int m_channel_id; //通道id，区分不同输入源 
        std::string m_stream_url; //输入流地址，可以是本地文件路径，也可以是网络 RTSP 地址
        MppDecoder* m_decoder; //解码器对象
        std::thread m_decode_thread; //解码线程
        std::atomic<bool> m_running; //线程控制标志
        uint64_t m_frame_counter; //帧计数器
        GlobalPool* m_pool; //指向全局线程池的指针 
        std::atomic<int>& m_active_count;     // 指向外部控制中心的计数器

};