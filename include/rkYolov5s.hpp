#ifndef RKYOLOV5S_H
#define RKYOLOV5S_H

#include "rknn_api.h"
#include <mutex>
#include "opencv2/core/core.hpp"
#include "postprocess.h"
#include "MppDecoder.h"

static void dump_tensor_attr(rknn_tensor_attr *attr);
static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz);
static unsigned char *load_model(const char *filename, int *model_size);
static int saveFloat(const char *file_name, float *output, int element_size);
struct input_data 
{
    int src_fd;       // 解码器给的底层钥匙
    MppBuffer src_buffer; // 解码帧底层 buffer，异步流水线中用引用计数保活
    int width;        // 原图宽
    int height;       // 原图高
    int hor_stride;   // 宽步长
    int ver_stride;   // 高步长
    MppFrame frame;
    // ======== 多路框架新增 ========
    int channel_id;      // 标记是哪一路视频
    uint64_t frame_id;   // 标记这路视频的第几帧 (用来排序)
    int64_t pts_us = -1; // 预留媒体时间戳，当前没有真实 PTS 时保持 -1

};

struct InferOutput 
{
    detect_result_group_t results; // 目标检测结果
    int channel_id;      // 标记是哪一路视频
    uint64_t frame_id;   // 标记这路视频的第几帧 (用来排序)
    int64_t pts_us = -1; // 预留媒体时间戳，当前没有真实 PTS 时保持 -1
    MppBuffer src_buffer; // 原始解码 DMA buffer，编码前由 RGA 读取
    int src_fd;
    int width;
    int height;
    int hor_stride;
    int ver_stride;
};

class rkYolov5s
{
private:
    int ret;
    std::mutex mtx;
    std::string model_path;
    unsigned char *model_data;

    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    //rknn_input inputs[1];

    rknn_tensor_mem* input_mems[1]; //外部存储
    int input_dma_fd;
    void* input_dma_buf;
    size_t input_dma_size;

    int channel, width, height;
    int img_width, img_height;

    float nms_threshold, box_conf_threshold;

public:
    int GetInputFd();
    rkYolov5s(const std::string &model_path);
    int init(rknn_context *ctx_in, bool isChild, int core_id = -1);
    rknn_context *get_pctx();
    //cv::Mat infer(cv::Mat &ori_img);
    InferOutput infer(input_data data);
    ~rkYolov5s();
};

#endif
