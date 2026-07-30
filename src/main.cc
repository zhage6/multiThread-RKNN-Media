#include <stdio.h>
#include <chrono>
#include <memory>
#include <thread>
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
    pipeline.SetInvalidOutputCallback([&channels](int channel_id)
    {
        if (channel_id >= 0 && channel_id < static_cast<int>(channels.size()) && channels[channel_id])
        {
            channels[channel_id]->OnInferDropped();
        }
    });
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

    if (!mosaic.Init(1920, 1080, 24))
    {
        printf("Mosaic/embedded RTSP initialization failed\n");
        aggregator.Stop();
        return -1;
    }
    channels.push_back(std::make_unique<VideoChannel>(0, "../test.h264", &pipeline,active_channels, &mosaic));
    channels.push_back(std::make_unique<VideoChannel>(1, "../test2.h264", &pipeline,active_channels, &mosaic));
    channels.push_back(std::make_unique<VideoChannel>(2, "../test3.h264", &pipeline,active_channels, &mosaic));
    channels.push_back(std::make_unique<VideoChannel>(3, "../test4.h264", &pipeline,active_channels, &mosaic));
//    channels.push_back(std::make_unique<VideoChannel>(4, "../test5.h264", &yolo,active_channels, &mosaic));
//    channels.push_back(std::make_unique<VideoChannel>(5, "../test6.h264", &yolo,active_channels, &mosaic));
    
    
    

    for (auto& ch : channels) 
    {
        ch->start();
    }
    printf("4路视频解码流水线全速启动...\n");
    printf("模型结果将直接进入聚合器，主线程等待流水线排空...\n");

    while (true) 
    {
        // 如果所有通道都已经关闭或异常退出，主线程结束
        if (active_channels == 0 && pipeline.DrainPendingCount() == 0)
        {
            printf("所有通道已关闭，推理结果已消费完，退出主循环。\n");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
