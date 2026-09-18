#include <thread>
#include <string>
#include <atomic>
#include <chrono>
#include "MppDecoder.h"
#include "MultiModelPipeline.h"

class VideoChannel
{
    public:
        VideoChannel(int channel_id, const std::string& stream_url, 
                    MultiModelPipeline* pipeline, std::atomic<int>& active_cnt);
        ~VideoChannel();
        void start();
        void Stop();
        void OnInferDropped();
        void OnFrameAggregated(uint64_t frame_id);

    private: 
        void DecodeLoop();
        void DecodeFileInput();
        void DecodeNetworkInput();
        bool WaitForDecodeCapacity();
        int m_channel_id; //通道id，区分不同输入源 
        std::string m_stream_url; //输入流地址，可以是本地文件路径，也可以是网络 RTSP 地址
        MppDecoder* m_decoder; //解码器对象
        std::thread m_decode_thread; //解码线程
        std::atomic<bool> m_running; //线程控制标志
        uint64_t m_frame_counter; //帧计数器
        MultiModelPipeline* m_pipeline; 
        std::atomic<int>& m_active_count;     // 指向外部控制中心的计数器

        std::chrono::steady_clock::time_point m_input_start_time;
        bool m_throttle_local_input;
        int m_input_fps;

        std::atomic<int> m_inflight_frames;
        int m_max_inflight_frames;

};
