#include <thread>
#include <string>
#include <atomic>
#include <iostream>
#include <memory>
#include <chrono>
#include "MppDecoder.h" // 你现有的 MPP 解码器
#include "MppEncoder.h"
#include "rknnPool.hpp"
#include "rkYolov5s.hpp"
#include "StreamPublisher.h"


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
                    GlobalPool* pool,std::atomic<int>& active_cnt);
        ~VideoChannel();
        void start();
        void Stop();
        void OnInferOutput(const InferOutput& out);

    private: 
        void DecodeLoop();
        void InitEncoder(int width,int height,MppFrameFormat fmt);
        void EncodeZeroCopy(const InferOutput& out);


        int m_channel_id; //通道id，区分不同输入源 
        std::string m_stream_url; //输入流地址，可以是本地文件路径，也可以是网络 RTSP 地址
        MppDecoder* m_decoder; //解码器对象
        std::thread m_decode_thread; //解码线程
        std::atomic<bool> m_running; //线程控制标志
        uint64_t m_frame_counter; //帧计数器
        GlobalPool* m_pool; //指向全局线程池的指针 
        std::atomic<int>& m_active_count;     // 指向外部控制中心的计数器

        //编码器特性
        RkMppEncoder* m_encoder; //编码器对象
        std::unique_ptr<StreamPublisher> m_publisher;
        std::map<uint64_t, InferOutput> m_reorder_buffer; // 重排池
        uint64_t m_expected_frame_id;
        std::mutex m_reorder_mtx;

        uint64_t m_encode_packet_counter;
        int64_t m_last_packet_pts;
        int m_output_fps;
        std::chrono::steady_clock::time_point m_stream_start_time;


};
