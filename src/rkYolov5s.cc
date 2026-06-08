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
#include "TimingLogger.h"

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
    model_data = nullptr;
    input_attrs = nullptr;
    output_attrs = nullptr;
    input_mems[0] = nullptr;
    input_dma_fd = -1;
    input_dma_buf = nullptr;
    input_dma_size = 0;
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
    input_dma_size = input_attrs[0].size_with_stride;
    if (dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, input_dma_size, &input_dma_fd, &input_dma_buf) < 0) {
        printf("RKNN input DMA32 buffer alloc fail!\n");
        return -1;
    }
    input_mems[0] = rknn_create_mem_from_fd(ctx, input_dma_fd, input_dma_buf, input_dma_size, 0);
    if (input_mems[0] == nullptr) {
        printf("RKNN input rknn_create_mem_from_fd fail!\n");
        dma_buf_free(input_dma_size, &input_dma_fd, input_dma_buf);
        input_dma_buf = nullptr;
        input_dma_size = 0;
        return -1;
    }
    
    ret = rknn_set_io_mem(ctx, input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("input_mems rknn_set_io_mem fail! ret=%d\n", ret);
        rknn_destroy_mem(ctx, input_mems[0]);
        input_mems[0] = nullptr;
        dma_buf_free(input_dma_size, &input_dma_fd, input_dma_buf);
        input_dma_buf = nullptr;
        input_dma_size = 0;
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
                          int dst_fd, int dst_w, int dst_h, int dst_w_stride, int dst_h_stride) 
{
// 1. 包装源内存 (MPP NV12)
    rga_buffer_t src_img = wrapbuffer_fd(src_fd, src_w, src_h, 
                                         RK_FORMAT_YCbCr_420_SP, 
                                         src_w_stride, src_h_stride);

    // 2. 包装目标内存 (NPU RGB)
    rga_buffer_t dst_img = wrapbuffer_fd(dst_fd, dst_w, dst_h, 
                                         RK_FORMAT_RGB_888, 
                                         dst_w_stride, dst_h_stride);

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
    auto total_start = timing::Clock::now();
    std::lock_guard<std::mutex> lock(mtx);
    auto lock_acquired = timing::Clock::now();


    int dst_fd = input_mems[0]->fd; 

    // 2. 【核心 0 拷贝动作】：呼叫 RGA！
    // 将 MPP 的 YUV(src_fd) 直接转码缩放进我的 RGB(dst_fd) 专属模具里
    int dst_w_stride = input_attrs[0].w_stride > 0 ? input_attrs[0].w_stride : width;
    auto rga_start = timing::Clock::now();
    int rga_ret = process_rga_zero_copy(
        data.src_fd, data.width, data.height, data.hor_stride, data.ver_stride,
        dst_fd, width, height, dst_w_stride, height // width 和 height 是 YOLO 模型的 640x640
    );
    auto rga_end = timing::Clock::now();

    if (data.frame != nullptr) {
        mpp_frame_deinit(&data.frame); //归还frame
        data.frame = nullptr;
    }
    

    detect_result_group_t detect_result_group;
    memset(&detect_result_group, 0, sizeof(detect_result_group));
    InferOutput out;
    out.channel_id = data.channel_id; // 贴上通道标签
    out.frame_id = data.frame_id;     // 贴上序号标签
    out.src_buffer = data.src_buffer;
    out.src_fd = data.src_fd;
    out.width = data.width;
    out.height = data.height;
    out.hor_stride = data.hor_stride;
    out.ver_stride = data.ver_stride;
    if (rga_ret != 0) {
        printf("RGA 搬运失败，丢弃该帧\n");
        out.results = detect_result_group;
        out.frame_id = -1;     // 贴上序号标签
        if (data.src_buffer) {
            mpp_buffer_put(data.src_buffer);
            out.src_buffer = nullptr;
        }
        timing::Log("rknn_infer_drop ch=%d frame=%llu reason=rga_pre_failed lock_wait_us=%lld rga_pre_us=%lld total_us=%lld",
                    data.channel_id,
                    static_cast<unsigned long long>(data.frame_id),
                    timing::UsBetween(total_start, lock_acquired),
                    timing::UsBetween(rga_start, rga_end),
                    timing::UsBetween(total_start, timing::Clock::now()));
        return out;
    }

    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 0;
    }

    // 模型推理/Model inference
    auto run_start = timing::Clock::now();
    ret = rknn_run(ctx, NULL);
    auto run_end = timing::Clock::now();
    auto outputs_get_start = timing::Clock::now();
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
    auto outputs_get_end = timing::Clock::now();

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
    auto post_start = timing::Clock::now();
    post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf, height, width,
                 box_conf_threshold, nms_threshold, pads, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
    auto post_end = timing::Clock::now();

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

    auto release_start = timing::Clock::now();
    ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
    auto release_end = timing::Clock::now();
    out.results = detect_result_group;
    timing::Log("rknn_infer ch=%d frame=%llu lock_wait_us=%lld rga_pre_us=%lld run_us=%lld outputs_get_us=%lld post_us=%lld release_us=%lld total_us=%lld boxes=%d",
                data.channel_id,
                static_cast<unsigned long long>(data.frame_id),
                timing::UsBetween(total_start, lock_acquired),
                timing::UsBetween(rga_start, rga_end),
                timing::UsBetween(run_start, run_end),
                timing::UsBetween(outputs_get_start, outputs_get_end),
                timing::UsBetween(post_start, post_end),
                timing::UsBetween(release_start, release_end),
                timing::UsBetween(total_start, timing::Clock::now()),
                detect_result_group.count);
    return out;
}

rkYolov5s::~rkYolov5s()
{
    deinitPostProcess();
    if (input_mems[0] != nullptr) 
    {
        rknn_destroy_mem(ctx, input_mems[0]);
    }
    if (input_dma_fd >= 0) {
        dma_buf_free(input_dma_size, &input_dma_fd, input_dma_buf);
        input_dma_buf = nullptr;
        input_dma_size = 0;
    }
    ret = rknn_destroy(ctx);

    if (model_data)
        free(model_data);

    if (input_attrs)
        free(input_attrs);
    if (output_attrs)
        free(output_attrs);
}
