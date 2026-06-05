#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <atomic>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_packet.h>
#include "mpp_packet_impl.h"
#include "dma_alloc.h"


// 定义码流回调函数的类型
// data: 编码后的 H264/H265 码流指针
// size: 码流大小
// is_keyframe: 是否是 I 帧 (方便上层做推流时的关键帧判断)
using PacketCallback = std::function<void(const uint8_t* data, size_t size, bool is_keyframe)>;

class RkMppEncoder {
public:
    RkMppEncoder();
    ~RkMppEncoder();
    MppBuffer GetFreeBuffer();

    // 1. 初始化编码器 (设置宽高、像素格式、编码格式 H264/H265 等)
    bool Init(int width, int height, MppFrameFormat fmt, MppCodingType type);

    // 2. 注册输出回调，硬件编码完成后会触发这个函数
    void SetOutputCallback(PacketCallback cb);

    // 3. 启动编码线程
    bool Start();

    // 4. 供上层调用的接口：压入一帧原始图像数据 (YUV/RGB)
    // 内部会从空闲队列取一个 DRM buffer，将数据拷贝进去，然后送给硬件
    bool PushFrame(const uint8_t* image_data, size_t data_size);//这个接口非0拷贝，不考虑使用
    
    bool PushBuffer(MppBuffer buffer);

    void RecycleBuffer(MppBuffer buffer);

    bool GetHeader(std::vector<uint8_t>& header);

    // 5. 停止并释放资源
    void Stop();

private:
    // 原 Demo 中的 enc_test_input 和 enc_test_output 逻辑移入这里
    void InputThreadFunc();
    void OutputThreadFunc();
    bool AllocateExternalBuffers(size_t frame_size, int count);
    void ReleaseExternalBuffers();

private:
    struct EncoderExternalBuffer {
        MppBuffer buffer;
        int fd;
        void* ptr;
        size_t size;
    };

    // MPP 核心上下文 (原 MpiEncTestData 中的核心成员)
    MppCtx ctx_;
    MppApi* mpi_;
    MppEncCfg cfg_;
    MppBufferGroup buf_grp_;

    // 基础参数
    int width_;
    int height_;
    MppFrameFormat fmt_;
    
    // C++ 线程管理
    std::thread input_thread_;
    std::thread output_thread_;
    std::atomic<bool> is_running_;

    // 回调函数
    PacketCallback on_packet_ready_;

    // Buffer 管理：替代原先的 priv->list_buf
    std::queue<MppBuffer> free_buffers_; 
    std::vector<EncoderExternalBuffer> external_buffers_;
    std::mutex mtx_;
    std::condition_variable cv_;
};
