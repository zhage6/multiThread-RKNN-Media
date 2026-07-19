#include <stdio.h>
#include <memory>
#include <vector>
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
#include "MosaicComposer.h"
#include "FrameResultAggregator.h"
#include "YoloModelAdapter.h"
#include "FrameTypes.h"
#include "MultiModelPipeline.h"
namespace dpool 
{
    constexpr size_t ThreadPool::WAIT_SECONDS;
}

int main(int argc, char **argv)
{
    char *model_name = NULL;
    if (argc != 3)
    {
        printf("Usage: %s <rknn model> <jpg> \n", argv[0]);
        return -1;
    }
    // 参数二，模型所在路径/The path where the model is located
    char* yolo_model = argv[1];
    char* face_model = argv[2];
    // 参数三, 视频/摄像头
    // char *video_path = argv[2];
 
    // 初始化rknn线程池/Initialize the rknn thread pool
    std::atomic<int> active_channels{4};
    int yoloThreadNum = 4;
    int faceThreadNum = 3;
    int in_flight_frames = 0;
    rknnPool<rkYolov5s, input_data, InferOutput> testPool(yolo_model, yoloThreadNum, std::vector<int>{0,1});
    rknnPool<rkYolov5s, input_data, InferOutput> facePool(face_model, faceThreadNum, std::vector<int>{2});
    if (testPool.init() != 0)
    {
        printf("rknnPool init fail!\n");
        return -1;
    }
    if (facePool.init() != 0) 
    {
        printf("facePool init fail!\n");
        return -1;
    }
    YoloModelAdapter yolo("yolo", &testPool);
    YoloModelAdapter face("face_yolo", &facePool);
    MosaicComposer mosaic;
    std::vector<std::shared_ptr<VideoChannel>> channels;
    FrameResultAggregator aggregator;
    MultiModelPipeline pipeline;
    pipeline.SetAggregator(&aggregator);
    pipeline.AddModel(&yolo, 1);
    pipeline.AddModel(&face, 3);
    aggregator.SetRequiredModels({"yolo"});
    aggregator.SetTimeout(std::chrono::milliseconds(120));
    aggregator.SetOutputCallback([&mosaic, &channels](const ComposedFrame& frame) 
    {
        int ch = frame.frame.channel_id;

        if (ch >= 0 && ch < static_cast<int>(channels.size()) && channels[ch]) 
        {
            channels[ch]->OnFrameAggregated(frame.frame.frame_id);
        }

        mosaic.Submit(frame);
    });
    aggregator.SetDropCallback([&channels](int channel_id, uint64_t frame_id)
    {
        if (channel_id >= 0 && channel_id < static_cast<int>(channels.size()) && channels[channel_id]) 
        {
            channels[channel_id]->OnFrameAggregated(frame_id);
        }
    });

    if (!aggregator.Start()) 
    {
        printf("FrameResultAggregator start failed\n");
        return -1;
    }

    mosaic.Init(1920, 1080, 24);
    channels.push_back(std::make_unique<VideoChannel>(0, "../test.h264", &pipeline,active_channels, &mosaic,&aggregator));
    channels.push_back(std::make_unique<VideoChannel>(1, "../test2.h264", &pipeline,active_channels, &mosaic,&aggregator));
    channels.push_back(std::make_unique<VideoChannel>(2, "../test3.h264", &pipeline,active_channels, &mosaic,&aggregator));
    channels.push_back(std::make_unique<VideoChannel>(3, "../test4.h264", &pipeline,active_channels, &mosaic,&aggregator));
//    channels.push_back(std::make_unique<VideoChannel>(4, "../test5.h264", &yolo,active_channels, &mosaic));
//    channels.push_back(std::make_unique<VideoChannel>(5, "../test6.h264", &yolo,active_channels, &mosaic));
    
    
    

    for (auto& ch : channels) 
    {
        ch->start();
    }
    printf("4路视频解码流水线全速启动...\n");
    printf("主线程开始接收推理结果并写入 MP4...\n");

    while (true) 
    {
        // 如果所有通道都已经关闭或异常退出，主线程结束
       if (active_channels == 0 && pipeline.PendingCount() == 0) 
        {
            printf("所有通道已关闭，推理结果已消费完，退出主循环。\n");
            break;
        }
        
        ModelOutput output; 
        // 从全局 NPU 池中阻塞/非阻塞获取推理完成的结果
        if (pipeline.TryGet(output))
        {
            // 过滤掉无效帧（比如解码错误或模型处理失败的帧）
           if (output.frame.frame_id == static_cast<uint64_t>(-1))
            {
                if (output.frame.channel_id >= 0 && output.frame.channel_id < channels.size()) 
                {
                    channels[output.frame.channel_id]->OnInferDropped();
                }
                if (output.frame.src_buffer) 
                {
                    mpp_buffer_put(output.frame.src_buffer);
                }
                continue;
            }

            // ===================================================================
            // 核心优化 4：O(1) 数组下标直接映射 + 防爆护盾
            // CPU 只需要一次加法指令即可找到目标对象，L1 Cache 命中率极高
            // ===================================================================
            if (output.frame.channel_id >= 0 && output.frame.channel_id < channels.size())
            {
                // 通道层不再重排，模型结果直接进入统一聚合器。
                channels[output.frame.channel_id]->OnModelOutput(output);
            }
            else
            {
                fprintf(stderr, "致命警告: 收到非法的 Channel ID: %d,越界丢弃!\n",
                        output.frame.channel_id);
                if (output.frame.src_buffer) 
                {
                    mpp_buffer_put(output.frame.src_buffer);
                }
            }
        }
    }

    // 4. 安全退出与资源释放
    printf("正在停止所有通道并释放硬件资源...\n");
    for (auto& ch : channels) {
        ch->Stop(); 
    }
    aggregator.Stop();

    printf("系统安全退出。\n");

    return 0;
}
