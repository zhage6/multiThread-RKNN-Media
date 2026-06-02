#include <stdio.h>
#include <memory>
#include <sys/time.h>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include <opencv2/videoio.hpp>
#include "rkYolov5s.hpp"
#include "rknnPool.hpp"
#include "postprocess.h"
#include "MppDecoder.h"
#include "StreamChannel.h"
namespace dpool 
{
    constexpr size_t ThreadPool::WAIT_SECONDS;
}

int main(int argc, char **argv)
{
    char *model_name = NULL;
    if (argc != 2)
    {
        printf("Usage: %s <rknn model> <jpg> \n", argv[0]);
        return -1;
    }
    // 参数二，模型所在路径/The path where the model is located
    model_name = (char *)argv[1];
    // 参数三, 视频/摄像头
    // char *video_path = argv[2];

    // 初始化rknn线程池/Initialize the rknn thread pool
    std::atomic<int> active_channels{2};
    int threadNum = 3;
    int in_flight_frames = 0;
    rknnPool<rkYolov5s, input_data, InferOutput> testPool(model_name, threadNum);
    if (testPool.init() != 0)
    {
        printf("rknnPool init fail!\n");
        return -1;
    }
    std::vector<std::shared_ptr<VideoChannel>> channels;
    channels.push_back(std::make_unique<VideoChannel>(0, "../test.h264", &testPool,active_channels));
    channels.push_back(std::make_unique<VideoChannel>(1, "../test2.h264", &testPool,active_channels));
    

    for (auto& ch : channels) 
    {
        ch->start();
    }
    printf("4路视频解码流水线全速启动...\n");
    printf("主线程开始接收推理结果并写入 MP4...\n");

    while (true) 
    {
        // 如果所有通道都已经关闭或异常退出，主线程结束
        if (active_channels == 0) {
            printf("所有通道已关闭，退出主循环。\n");
            break;
        }
        
        InferOutput out;
        // 从全局 NPU 池中阻塞/非阻塞获取推理完成的结果
        if (testPool.get(out) == 0) 
        {
            // 过滤掉无效帧（比如解码错误或模型处理失败的帧）
            if(out.frame_id == -1) {
                if (out.src_buffer) {
                    mpp_buffer_put(out.src_buffer);
                }
                continue;
            }

            // ===================================================================
            // 核心优化 4：O(1) 数组下标直接映射 + 防爆护盾
            // CPU 只需要一次加法指令即可找到目标对象，L1 Cache 命中率极高
            // ===================================================================
            if (out.channel_id >= 0 && out.channel_id < channels.size()) 
            {
                // 瞬间将带有元数据和 dma_fd 的包投递回对应的通道进行重排和编码
                channels[out.channel_id]->OnInferOutput(out);
            } 
            else 
            {
                // 防段错误 (Segmentation Fault) 保护
                fprintf(stderr, "致命警告: 收到非法的 Channel ID: %d,越界丢弃!\n", out.channel_id);
                if (out.src_buffer) {
                    mpp_buffer_put(out.src_buffer);
                }
            }
        }
    }

    // 4. 安全退出与资源释放
    printf("正在停止所有通道并释放硬件资源...\n");
    for (auto& ch : channels) {
        ch->Stop(); 
    }

    printf("系统安全退出。\n");

    return 0;
}
