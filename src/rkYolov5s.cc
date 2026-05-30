#include <stdio.h>
#include <mutex>
#include "rknn_api.h"
#include "postprocess.h"
#include "preprocess.h"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "coreNum.hpp"
#include "rkYolov5s.hpp"

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    std::string shape_str = attr->n_dims < 1 ? "" : std::to_string(attr->dims[0]);
    for (int i = 1; i < attr->n_dims; ++i)
    {
        shape_str += ", " + std::to_string(attr->dims[i]);
    }

    // printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, w_stride = %d, size_with_stride=%d, fmt=%s, "
    //        "type=%s, qnt_type=%s, "
    //        "zp=%d, scale=%f\n",
    //        attr->index, attr->name, attr->n_dims, shape_str.c_str(), attr->n_elems, attr->size, attr->w_stride,
    //        attr->size_with_stride, get_format_string(attr->fmt), get_type_string(attr->type),
    //        get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(const char *filename, int *model_size)
{
    FILE *fp;
    unsigned char *data;

    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

static int saveFloat(const char *file_name, float *output, int element_size)
{
    FILE *fp;
    fp = fopen(file_name, "w");
    for (int i = 0; i < element_size; i++)
    {
        fprintf(fp, "%.6f\n", output[i]);
    }
    fclose(fp);
    return 0;
}

rkYolov5s::rkYolov5s(const std::string &model_path)
{
    this->model_path = model_path;
    nms_threshold = NMS_THRESH;      // 默认的NMS阈值
    box_conf_threshold = BOX_THRESH; // 默认的置信度阈值
}

int rkYolov5s::init(rknn_context *ctx_in, bool share_weight)
{
    printf("Loading model...\n");
    int model_data_size = 0;
    model_data = load_model(model_path.c_str(), &model_data_size);
    // 模型参数复用/Model parameter reuse 多线程下使用
    if (share_weight == true)
        ret = rknn_dup_context(ctx_in, &ctx);
    else
        ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }

    // 设置模型绑定的核心/Set the core of the model that needs to be bound
    rknn_core_mask core_mask;
    switch (get_core_num())
    {
    case 0:
        core_mask = RKNN_NPU_CORE_0;
        break;
    case 1:
        core_mask = RKNN_NPU_CORE_1;
        break;
    case 2:
        core_mask = RKNN_NPU_CORE_2;
        break;
    }
    ret = rknn_set_core_mask(ctx, core_mask);
    if (ret < 0)
    {
        printf("rknn_init core error ret=%d\n", ret);
        return -1;
    }

    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version, version.drv_version);

    // 获取模型输入输出参数/Obtain the input and output parameters of the model
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // 设置输入参数/Set the input parameters
    input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_mems[0] = rknn_create_mem(ctx,input_attrs[0].size_with_stride);
    
    ret = rknn_set_io_mem(ctx, input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        return -1;
    }
    // 设置输出参数/Set the output parameters
    output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        dump_tensor_attr(&(output_attrs[i]));
    }

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        channel = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        width = input_attrs[0].dims[3];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        height = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }
    printf("model input height=%d, width=%d, channel=%d\n", height, width, channel);

    // memset(inputs, 0, sizeof(inputs));
    // inputs[0].index = 0;
    // inputs[0].type = RKNN_TENSOR_UINT8;
    // inputs[0].size = width * height * channel;
    // inputs[0].fmt = RKNN_TENSOR_NHWC;
    // inputs[0].pass_through = 0;

    return 0;
}

int rkYolov5s::GetInputFd()
{
    if (input_mems[0] != nullptr) 
    {
        return input_mems[0]->fd;
    }
    return -1;
}

rknn_context *rkYolov5s::get_pctx()
{
    return &ctx;
}
/**
 * @brief 使用 RGA 将 MPP 的 NV12 物理内存，直接转码+缩放至 NPU 的 RGB 物理内存
 * * @param src_fd       MPP 解码吐出的底层钥匙
 * @param src_w        原视频真实宽度 (如 1920)
 * @param src_h        原视频真实高度 (如 1080)
 * @param src_w_stride MPP 硬件对齐后的宽度步长 (极重要！通常是 256 的整数倍)
 * @param src_h_stride MPP 硬件对齐后的高度步长
 * @param dst_fd       RKNN 申请的 NPU 输入内存钥匙
 * @param dst_w        YOLO 模型的输入宽度 (如 640)
 * @param dst_h        YOLO 模型的输入高度 (如 640)
 * @return int         0 表示成功，<0 表示失败
 */
int process_rga_zero_copy(int src_fd, int src_w, int src_h, int src_w_stride, int src_h_stride,
                          int dst_fd, int dst_w, int dst_h) 
{
// 1. 包装源内存 (MPP NV12)
    rga_buffer_t src_img = wrapbuffer_fd(src_fd, src_w, src_h, 
                                         RK_FORMAT_YCbCr_420_SP, 
                                         src_w_stride, src_h_stride);

    // 2. 包装目标内存 (NPU RGB)
    rga_buffer_t dst_img = wrapbuffer_fd(dst_fd, dst_w, dst_h, 
                                         RK_FORMAT_RGB_888, 
                                         dst_w, dst_h);

    // 3. 一键拉伸 + 转码！
    // RGA 非常聪明，它会自动读取 src 和 dst 里面的宽高和格式，直接完成所有转换
    
    IM_STATUS status = imresize(src_img, dst_img);
        
    if (status != IM_STATUS_SUCCESS) {
        printf("RGA 硬件转换失败: %s\n", imStrError(status));
        return -1;
    }
    return 0;
}



InferOutput rkYolov5s::infer(input_data data)
{
    std::lock_guard<std::mutex> lock(mtx);


    int dst_fd = input_mems[0]->fd; 

    // 2. 【核心 0 拷贝动作】：呼叫 RGA！
    // 将 MPP 的 YUV(src_fd) 直接转码缩放进我的 RGB(dst_fd) 专属模具里
    int rga_ret = process_rga_zero_copy(
        data.src_fd, data.width, data.height, data.hor_stride, data.ver_stride,
        dst_fd, width, height // width 和 height 是 YOLO 模型的 640x640
    );

    // 2. OpenCV 提取原图并做色彩转换
    cv::Mat bgr_original;
    if (rga_ret == 0 && data.frame != nullptr) 
    {
        MppBuffer buffer = mpp_frame_get_buffer(data.frame);
        void* mpp_va = mpp_buffer_get_ptr(buffer);

        // 使用对齐跨距构造 YUV，再裁剪成真实尺寸，最后转为 BGR
        cv::Mat yuv_aligned(data.ver_stride * 3 / 2, data.hor_stride, CV_8UC1, mpp_va);
        cv::Mat yuv_cropped = yuv_aligned(cv::Rect(0, 0, data.width, data.height * 3 / 2));
        cv::cvtColor(yuv_cropped, bgr_original, cv::COLOR_YUV2BGR_NV12);
    }


    if (data.frame != nullptr) {
        mpp_frame_deinit(&data.frame); //归还frame
        data.frame = nullptr;
    }
    

    detect_result_group_t detect_result_group;
    memset(&detect_result_group, 0, sizeof(detect_result_group));
    InferOutput out;
    out.channel_id = data.channel_id; // 贴上通道标签
    out.frame_id = data.frame_id;     // 贴上序号标签
    if (rga_ret != 0) {
        printf("RGA 搬运失败，丢弃该帧\n");
        out.results = detect_result_group;
        out.frame_id = -1;     // 贴上序号标签
        return out;
    }

    // cv::Mat img;
    // cv::cvtColor(orig_img, img, cv::COLOR_BGR2RGB);
    // img_width = img.cols;
    // img_height = img.rows;

    // BOX_RECT pads;
    // memset(&pads, 0, sizeof(BOX_RECT));
    // cv::Size target_size(width, height);
    // cv::Mat resized_img(target_size.height, target_size.width, CV_8UC3);
    // // 计算缩放比例/Calculate the scaling ratio
    // float scale_w = (float)target_size.width / img.cols;
    // float scale_h = (float)target_size.height / img.rows;

    // // 图像缩放/Image scaling
    // if (img_width != width || img_height != height)
    // {
    //     // rga
    //     rga_buffer_t src;
    //     rga_buffer_t dst;
    //     memset(&src, 0, sizeof(src));
    //     memset(&dst, 0, sizeof(dst));
    //     ret = resize_rga(src, dst, img, resized_img, target_size);
    //     if (ret != 0)
    //     {
    //         fprintf(stderr, "resize with rga error\n");
    //     }
    //     /*********
    //     // opencv
    //     float min_scale = std::min(scale_w, scale_h);
    //     scale_w = min_scale;
    //     scale_h = min_scale;
    //     letterbox(img, resized_img, pads, min_scale, target_size);
    //     *********/
    //     inputs[0].buf = resized_img.data;
    // }
    // else
    // {
    //     inputs[0].buf = img.data;
    // }

    // rknn_inputs_set(ctx, io_num.n_input, inputs);

    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 0;
    }

    // 模型推理/Model inference
    ret = rknn_run(ctx, NULL);
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);

    // 计算缩放比例 (模型输入大小 / 原始视频帧大小)
    float scale_w = (float)width / data.width;
    float scale_h = (float)height / data.height;

    BOX_RECT pads;
    memset(&pads, 0, sizeof(BOX_RECT)); // 你原代码没用 letterbox，pad 为 0

    // 后处理/Post-processing
    //detect_result_group_t detect_result_group;
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }
    post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf, height, width,
                 box_conf_threshold, nms_threshold, pads, scale_w, scale_h, out_zps, out_scales, &detect_result_group);

    // // 绘制框体/Draw the box
    // char text[256];
    // for (int i = 0; i < detect_result_group.count; i++)
    // {
    //     detect_result_t *det_result = &(detect_result_group.results[i]);
    //     sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
    //     // 打印预测物体的信息/Prints information about the predicted object
    //     // printf("%s @ (%d %d %d %d) %f\n", det_result->name, det_result->box.left, det_result->box.top,
    //     //        det_result->box.right, det_result->box.bottom, det_result->prop);
    //     int x1 = det_result->box.left;
    //     int y1 = det_result->box.top;
    //     int x2 = det_result->box.right;
    //     int y2 = det_result->box.bottom;
    //     rectangle(orig_img, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(256, 0, 0, 256), 3);
    //     putText(orig_img, text, cv::Point(x1, y1 + 12), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(255, 255, 255));
    // }

    // 5. 画框 (画在刚转出来的 bgr_original 上)
    if (!bgr_original.empty()) {
        for (int i = 0; i < detect_result_group.count; i++) {
            auto& res = detect_result_group.results[i];
            cv::rectangle(bgr_original, cv::Point(res.box.left, res.box.top), 
                          cv::Point(res.box.right, res.box.bottom), cv::Scalar(0, 255, 0), 2);
            std::string label = "Class " + std::string(res.name);
            cv::putText(bgr_original, label, cv::Point(res.box.left, res.box.top - 5), 
                        cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
        }
    }

    ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
    out.results = detect_result_group;
    out.image = bgr_original;
    return out;
}

rkYolov5s::~rkYolov5s()
{
    deinitPostProcess();
    if (input_mems[0] != nullptr) 
    {
        rknn_destroy_mem(ctx, input_mems[0]);
    }
    ret = rknn_destroy(ctx);

    if (model_data)
        free(model_data);

    if (input_attrs)
        free(input_attrs);
    if (output_attrs)
        free(output_attrs);
}
