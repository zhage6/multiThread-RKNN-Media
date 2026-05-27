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
    int width;        // 原图宽
    int height;       // 原图高
    int hor_stride;   // 宽步长
    int ver_stride;   // 高步长
    MppFrame frame;
};

struct InferOutput 
{
    detect_result_group_t results; // 目标检测结果
    cv::Mat image;                 // 画好框的高清原图
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

    int channel, width, height;
    int img_width, img_height;

    float nms_threshold, box_conf_threshold;

public:
    int GetInputFd();
    rkYolov5s(const std::string &model_path);
    int init(rknn_context *ctx_in, bool isChild);
    rknn_context *get_pctx();
    //cv::Mat infer(cv::Mat &ori_img);
    InferOutput infer(input_data data);
    ~rkYolov5s();
};

#endif