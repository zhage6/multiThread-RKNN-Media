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

    // ==========================================
    // 1. 定义多路视频写入器和重排缓冲区
    // ==========================================
    // 存放每个通道对应的 OpenCV VideoWriter
    std::map<int, cv::VideoWriter> writers;
    
    // 存放每个通道当前期待写入的“下一帧序号”（严格保证顺序）
    std::map<int, uint64_t> expected_frame_ids;
    
    // 重排缓冲区：存放那些“提前算完但还轮不到它写入”的帧
    // 结构: map<通道号, map<帧序号, 图像Mat>>
    std::map<int, std::map<uint64_t, cv::Mat>> reorder_buffers;

    // ==========================================
    // 2. 主循环：全速拉取与精准分发写入
    // ==========================================
    while (true) 
    {
        if(active_channels == 0)
            break;
        InferOutput out;
        
        // 阻塞获取 NPU 池子里的处理结果
        if (testPool.get(out) == 0 ) 
        {
            if(out.frame_id == -1)
            {
                // 这是一帧搬运失败的特殊标记，直接丢弃，不进入重排逻辑
                printf("收到一帧搬运失败的结果，已丢弃！\n");
                continue;
            }
            int cid = out.channel_id;
            uint64_t fid = out.frame_id;
            
            // 【首次初始化该通道的录像机】
            if (writers.find(cid) == writers.end()) {
                std::string filename = "result_channel_" + std::to_string(cid) + ".mp4";
                // 注意：这里需要确保 out.image 不为空，且宽高正确
                writers[cid].open(filename, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), 
                                  25.0, cv::Size(out.image.cols, out.image.rows));
                
                // 初始化该通道期待的第一帧序号
                expected_frame_ids[cid] = 0; // 假设你的帧号是从 0 开始的
                printf("通道 %d 的录像机已启动！\n", cid);
            }

            // 【将当前帧放入该通道的重排缓冲区】
            // (注意：这里假设 out.image 是独立内存，如果用的是浅拷贝，需要 out.image.clone())
            reorder_buffers[cid][fid] = out.image.clone();

            // 【顺序消费逻辑】：检查缓冲区里有没有当前期待的那一帧
            while (reorder_buffers[cid].count(expected_frame_ids[cid]) > 0) 
            {
                uint64_t current_expected = expected_frame_ids[cid];
                
                // 1. 拿出来写入 MP4
                writers[cid].write(reorder_buffers[cid][current_expected]);
                
                // 2. 写入完毕，从缓冲区销毁，释放内存
                reorder_buffers[cid].erase(current_expected);
                
                // 3. 期待值 + 1，准备写下一帧！
                expected_frame_ids[cid]++;
                
                // 如果恰好后面几帧（比如 101, 102）早就等在缓冲区里了，
                // 这个 while 循环会一次性把它们全部顺畅地写进去！
            
            }
            printf("目前通道%d缓存区已经缓存了%d帧等待写入\n", cid, (int)reorder_buffers[cid].size());
            // 【防爆内存保护】如果某个通道的一帧丢失了(导致后面的帧一直卡在缓冲区写不进去)
            // 设定一个阈值，如果积压超过 30 帧，强制跳过丢失的帧
            if (reorder_buffers[cid].size() > 20) {
                printf("警告：通道 %d 出现严重丢帧！强制向前推进序号。\n", cid);
                // 把期待帧号强制设置为缓冲区里最小的那个帧号
                expected_frame_ids[cid] = reorder_buffers[cid].begin()->first;
            }
        }
    }

    // 释放资源...
    for (auto& pair : writers) {
        pair.second.release();
    }

    return 0;
}