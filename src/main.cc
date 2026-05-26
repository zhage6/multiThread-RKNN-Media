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
int main(int argc, char **argv)
{
    char *model_name = NULL;
    if (argc != 3)
    {
        printf("Usage: %s <rknn model> <jpg> \n", argv[0]);
        return -1;
    }
    // 参数二，模型所在路径/The path where the model is located
    model_name = (char *)argv[1];
    // 参数三, 视频/摄像头
    char *video_path = argv[2];

    // 初始化rknn线程池/Initialize the rknn thread pool
    int threadNum = 3;
    rknnPool<rkYolov5s, input_data, detect_result_group_t> testPool(model_name, threadNum);
    if (testPool.init() != 0)
    {
        printf("rknnPool init fail!\n");
        return -1;
    }
    MppDecoder decoder;
    decoder.Init(
    // ========== 第 1 个参数：Lambda 回调函数 ==========
    [&](int src_fd, int width, int height, int hor_stride, int ver_stride) {
        // 这里是 Lambda 的函数体 (注意有大括号 {})
        input_data data = {src_fd, width, height, hor_stride, ver_stride};
        testPool.put(data); 
    }, // <-- 注意这里有一个逗号！用来分隔第一个和第二个参数

    // ========== 第 2 个参数：视频编码类型 ==========
        MPP_VIDEO_CodingAVC
    );
    FILE* fp = fopen(video_path, "rb");
    if (!fp) {
        printf("打开视频文件失败！请检查路径。\n");
        return -1;
    }
    unsigned char buffer[4096]; // 每次给解码器喂 4KB 的压缩数据包
    int frames_sent = 0;        // 记录丢进线程池的总帧数

    printf("流水线全速启动...\n");

    // ==========================================
    // 4. 极限狂飙主循环
    // ==========================================
    while (!feof(fp)) {
        // A. 读一包压缩字节流
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
        if (bytes_read > 0) {
            // B. 塞进解码器肚子，并强制它消化
            // 解码器内部如果攒够了一帧，就会自动触发上面的 Lambda 回调
            decoder.DecodePacket(buffer, bytes_read);
            decoder.FlushDecoder(); 
        }

        // C. 滑动窗口阻塞机制 (防止 MPP 解码太快，撑爆内存)
        // 你的原版经典保命逻辑：只要队列里的任务超过了线程数，就必须先拿走一个结果才准继续放
        if (frames_sent >= threadNum) {
            detect_result_group_t results;
            if (testPool.get(results) == 0) 
            {
                // 成功拿到了 NPU 的检测结果！
                printf("第帧处理完毕！检测到 %d 个目标。\n", results.count);
                // 这里你可以打印坐标：results.results[0].box.left 等
                // 【注意】：因为是 0 拷贝，此时你手里没有 cv::Mat 图片，无法直接用 imshow 画框！
                // 真正的生产环境，这里通常是把坐标打成 JSON 发给后端，或者配合 DRM/VO 直接在屏幕上画 UI 框。
            }
        }
        
        frames_sent++;
    }
    detect_result_group_t results;
    while (testPool.get(results) == 0) {
        printf("收尾排空队列... 检测到 %d 个目标。\n", results.count);
    }

    fclose(fp);
    printf("全链路运行结束！\n");

    


    // cv::namedWindow("Camera FPS");
    // // cv::VideoCapture capture;
    // // if (strlen(vedio_name) == 1)
    // //     capture.open((int)(vedio_name[0] - '0'));
    // // else
    // //     capture.open(vedio_name);
    // FILE* fp = fopen(video_path, "rb");
    // if (!fp) {
    //     printf("打开视频文件失败！请检查路径。\n");
    //     return -1;
    // }

    // struct timeval time;
    // gettimeofday(&time, nullptr);
    // auto startTime = time.tv_sec * 1000 + time.tv_usec / 1000;

    // int frames = 0;
    // auto beforeTime = startTime;
    // while (capture.isOpened())
    // {
    //     cv::Mat img;
    //     if (capture.read(img) == false)
    //         break;
    //     if (testPool.put(img) != 0)
    //         break;

    //     if (frames >= threadNum && testPool.get(img) != 0) //都使用了引用机制,没事
    //         break;
    //     cv::imshow("Camera FPS", img);
    //     if (cv::waitKey(1) == 'q') // 延时1毫秒,按q键退出/Press q to exit
    //         break;
    //     frames++;

    //     if (frames % 120 == 0)
    //     {
    //         gettimeofday(&time, nullptr);
    //         auto currentTime = time.tv_sec * 1000 + time.tv_usec / 1000;
    //         printf("120帧内平均帧率:\t %f fps/s\n", 120.0 / float(currentTime - beforeTime) * 1000.0);
    //         beforeTime = currentTime;
    //     }
    // }

    // // 清空rknn线程池/Clear the thread pool
    // while (true)
    // {
    //     cv::Mat img;
    //     if (testPool.get(img) != 0)
    //         break;
    //     cv::imshow("Camera FPS", img);
    //     if (cv::waitKey(1) == 'q') // 延时1毫秒,按q键退出/Press q to exit
    //         break;
    //     frames++;
    // }

    // gettimeofday(&time, nullptr);
    // auto endTime = time.tv_sec * 1000 + time.tv_usec / 1000;

    // printf("Average:\t %f fps/s\n", float(frames) / float(endTime - startTime) * 1000.0);

    return 0;
}