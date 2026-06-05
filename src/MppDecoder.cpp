#include "MppDecoder.h"
#include <unistd.h>
#include <iostream>

// --- 构造函数：全部初始化为空 ---
MppDecoder::MppDecoder() : 
    m_ctx(nullptr), m_mpi(nullptr), m_frm_grp(nullptr),
    m_initialized(false), m_src_width(0), m_src_height(0) 
{
}

// --- 析构函数：自动触发清理 ---
MppDecoder::~MppDecoder() {
    ReleaseAll();
}


// --- 初始化解码器 ---
bool MppDecoder::Init(FrameCallback callback, MppCodingType type) 
{
    m_callback = callback;
    m_type = type;

    RK_U32 need_split = 1;
    MppParam param = &need_split;

    // 1. 创建硬件上下文
    if (mpp_create(&m_ctx, &m_mpi) != MPP_OK) {
        std::cerr << "mpp_create 失败" << std::endl;
        return false;
    }

    // 2. 开启分帧解析（核心：应对网络传输的不完整 H.264 碎片）
    if (m_mpi->control(m_ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, param) != MPP_OK) {
        return false;
    }

    // 3. 初始化为解码模式
    if (mpp_init(m_ctx, MPP_CTX_DEC, m_type) != MPP_OK) {
        return false;
    }

    // 4. 创建一个空的 Packet 壳子（不分配内存，实现输入端 0 拷贝）
    // if (mpp_packet_init(&m_packet, nullptr, 0) != MPP_OK) {
    //     return false;
    // }

    m_initialized = true;
    std::cout << "MPP 硬件解码器初始化成功！" << std::endl;
    return true;
}

// --- 清理函数：释放所有硬件和 DMA 内存 ---
void MppDecoder::ReleaseAll() {
    // 1. 释放 MPP 数据包壳子
    // if (m_packet) {
    //     mpp_packet_deinit(&m_packet);
    //     m_packet = nullptr;
    // }
    // 2. 销毁 MPP 解码器硬件上下文
    if (m_ctx) {
        mpp_destroy(m_ctx);
        m_ctx = nullptr;
    }
    // 3. 释放 MPP 内存池组
    if (m_frm_grp) {
        mpp_buffer_group_put(m_frm_grp);
        m_frm_grp = nullptr;
    }
    
    // 4. 最重要：释放我们自己申请的 24 块底层 DMA 物理内存
    for (auto& pair : buffer_map) {
        ExternalBuffer& ext_buf = pair.second;
        // 释放 RGA 句柄
        if (ext_buf.rga_handle > 0) {
            releasebuffer_handle(ext_buf.rga_handle);
        }
        // 调用我们自己写的 DMA 释放函数，归还给 Linux 内核
        dma_buf_free(ext_buf.buf_size, &ext_buf.src_dma_fd, ext_buf.src_buf);
    }
    buffer_map.clear();
    std::cout << "MppDecoder 资源已彻底释放。" << std::endl;
}

// --- 动态分配 24 块外部 DMA 内存池（Mode 3 核心） ---
int MppDecoder::AllocateExternalBuffers(size_t buf_size, int width, int height) {
    MPP_RET ret;
    
    // 1. 创建一个完全空的、被标记为“外部DMA”的内存组
    ret = mpp_buffer_group_get_external(&m_frm_grp, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret != MPP_OK) return -1;

    // 2. 主控循环：自己去 Linux 系统申请 24 把物理内存钥匙
    for (int i = 0; i < 24; i++) {
        ExternalBuffer ext_buf;
        memset(&ext_buf, 0, sizeof(ExternalBuffer));
        ext_buf.buf_size = buf_size;

        // 调用我们刚才写的 dma_alloc.h 申请物理内存
        if (dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, buf_size, &ext_buf.src_dma_fd, &ext_buf.src_buf) < 0) {
            std::cerr << "DMA 内存申请失败!" << std::endl;
            return -1;
        }

        // 提前让 RGA 认识这把钥匙（Import 到 RGA 驱动中）
        ext_buf.rga_handle = importbuffer_fd(ext_buf.src_dma_fd, buf_size);
        // 提前包装成 RGA 的图像结构体 (NV12 格式)
        // 注意：硬件一般对齐为 256，这里必须使用 m_hor_stride 和 m_ver_stride
        ext_buf.rga_buf = wrapbuffer_handle(ext_buf.rga_handle, width, height, RK_FORMAT_YCbCr_420_SP);

        // 3. 核心标志动作：把申请到的 fd 强行“Commit(提交)”进 MPP 的内存组里
        MppBufferInfo commit_info;
        memset(&commit_info, 0, sizeof(commit_info));
        commit_info.type = MPP_BUFFER_TYPE_DMA_HEAP;
        commit_info.size = buf_size;
        commit_info.fd   = ext_buf.src_dma_fd;
        commit_info.ptr  = ext_buf.src_buf; // 虚拟地址供参考

        ret = mpp_buffer_commit(m_frm_grp, &commit_info);
        if (ret != MPP_OK) 
        {
            std::cerr << "向 MPP 提交外部内存失败!" << std::endl;
            return -1;
        }

        // 把这把钥匙存进字典，方便以后 RGA 直接拿来用
        buffer_map[ext_buf.src_dma_fd] = ext_buf;
    }

    std::cout << "成功为解码器分配并注入 24 块外部 DMA 内存！" << std::endl;
    return 0;
}

// --- 核心加工接口：处理每一包 H.264 数据 ---
void MppDecoder::DecodePacket(const uint8_t* data, size_t size) 
{
    if (!m_initialized || !data || size == 0) return;

    MppPacket packet = nullptr;
    mpp_packet_init(&packet, (void*)data, size);
    mpp_packet_set_pos(packet, (void*)data);
    mpp_packet_set_length(packet, size);

    // ==========================================
    // 核心修复：加入背压重试机制（防止 CPU 把硬件撑爆）
    // ==========================================
    MPP_RET ret = MPP_OK;
    int retry_count = 0;
    
    do 
    {
        // 尝试把快递单塞进硬件队列
        ret = m_mpi->decode_put_packet(m_ctx, packet);
        
        if (ret == MPP_OK) {
            break; // 塞包成功，顺利跳出循环！
        }
        
        // 如果走到这里，说明硬件返回了失败（大概率是 MPP_ERR_BUFFER_FULL）
        // 应对策略：强迫硬件去消费（Flush），腾出肚子，稍微等一下，然后重试！
        FlushDecoder(); 
        usleep(2000); // 挂起当前 CPU 线程 2 毫秒，不要死循环空转
        
        retry_count++;
        if (retry_count > 200)
        {
            // 如果等了 400 毫秒还没塞进去，说明硬件彻底卡死了
            std::cerr << "致命错误：解码器严重卡死，塞包连续超时！" << std::endl;
            break; 
        }
    } while (ret != MPP_OK);

    // 塞包成功后，例行呼叫一次取货
    if (ret == MPP_OK) {
        FlushDecoder(); 
    }

    // 硬件接管了数据后，释放当前线程的局部外壳
    mpp_packet_deinit(&packet);
}

void MppDecoder::FlushDecoder() 
{
    MppFrame frame = nullptr;
    
    // 死循环取货，直到硬件说“我肚子里暂时没货了（TIMEOUT）”
    while (true) {
        MPP_RET ret = m_mpi->decode_get_frame(m_ctx, &frame);
        
        if (ret == MPP_ERR_TIMEOUT || frame == nullptr) {
            break; // 榨干了，退出循环
        }

        if (ret == MPP_OK && frame != nullptr) {
            // 【情况 A】：遇到分辨率改变（第一帧）
            if (mpp_frame_get_info_change(frame)) {
                m_src_width  = mpp_frame_get_width(frame);
                m_src_height = mpp_frame_get_height(frame);
                m_hor_stride = mpp_frame_get_hor_stride(frame);
                m_ver_stride = mpp_frame_get_ver_stride(frame);
                
                if (m_frm_grp == nullptr) {
                    AllocateExternalBuffers(mpp_frame_get_buf_size(frame), m_hor_stride, m_ver_stride);
                }
                
                m_mpi->control(m_ctx, MPP_DEC_SET_EXT_BUF_GROUP, m_frm_grp);
                m_mpi->control(m_ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            } 
            // 【情况 B】：拿到正常画面，抛出 src_fd 回调
            else if (mpp_frame_get_errinfo(frame) == 0 && mpp_frame_get_discard(frame) == 0) {
                int src_fd = mpp_buffer_get_fd(mpp_frame_get_buffer(frame));
                if (m_callback) {
                    m_callback(src_fd, m_src_width, m_src_height, m_hor_stride, m_ver_stride, frame);
                }
            }

            // // 归还帧引用
            // mpp_frame_deinit(&frame); //不能立马释放
            // frame = nullptr;
            else 
            {
                mpp_frame_deinit(&frame); // 废废帧当场释放
            }
        }
    }
}