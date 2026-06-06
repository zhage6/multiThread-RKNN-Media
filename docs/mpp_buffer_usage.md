# MPP Buffer 使用说明

本文基于 `/usr/include/rockchip/mpp_buffer.h` 整理，重点解释 `MppBuffer` 和 `MppBufferGroup` 的用法，以及它们在硬件编解码、RGA、DMA buffer 场景中的作用。

## 1. MppBuffer 是什么

`MppBuffer` 是 Rockchip MPP 对一块内存的抽象。

这块内存可以来自：

- 普通 malloc 内存
- ION
- DRM
- DMA_HEAP
- 外部传入的 dma-buf fd

对于硬件编解码来说，`MppBuffer` 通常不是普通 CPU 内存，而是一块硬件模块可以访问的 DMA buffer。MPP 编码器、解码器、RGA、RKNN 等模块常常通过 fd 共享这类内存。

可以把 `MppBuffer` 理解成：

```text
MppBuffer = MPP 认识的一块 buffer 句柄
```

而底层真实内存可能是：

```text
dma-buf fd / ion fd / drm buffer / malloc ptr
```

## 2. MppBufferGroup 是什么

`MppBufferGroup` 是一组 `MppBuffer` 的管理器。

它主要负责：

- 管理 buffer 池
- 管理 buffer 引用计数
- 从池中取 buffer
- 把 buffer 放回池
- 限制 buffer 的大小和数量
- 管理外部提交进来的 buffer

可以把它理解成：

```text
MppBufferGroup = MPP 的 buffer pool
```

一个典型关系是：

```text
MppBufferGroup
  ├── MppBuffer A
  ├── MppBuffer B
  ├── MppBuffer C
  └── MppBuffer D
```

## 3. 两种工作模式

`mpp_buffer.h` 里把 `MppBufferGroup` 分成两种模式：

```cpp
typedef enum {
    MPP_BUFFER_INTERNAL,
    MPP_BUFFER_EXTERNAL,
    MPP_BUFFER_MODE_BUTT,
} MppBufferMode;
```

### 3.1 Internal 模式

Internal 模式表示 buffer 由 MPP 内部分配。

典型流程：

```cpp
MppBufferGroup group = nullptr;
mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DMA_HEAP);

MppBuffer buffer = nullptr;
mpp_buffer_get(group, &buffer, size);

// 使用 buffer

mpp_buffer_put(buffer);
mpp_buffer_group_put(group);
```

这种方式简单，MPP 自己负责分配和回收。

适合：

- 快速 demo
- 不关心底层 fd 来源
- 不需要严格控制 DMA 地址范围
- 不需要和特殊硬件模块共享内存

风险：

- 你不一定能控制 buffer 来自哪个 heap
- 在 RK3588 某些 RGA 场景中，可能遇到 4G 地址以上内存访问问题

### 3.2 External 模式

External 模式表示 buffer 由应用层或外部模块分配，然后提交给 MPP 管理。

典型流程：

```cpp
MppBufferGroup group = nullptr;
mpp_buffer_group_get_external(&group, MPP_BUFFER_TYPE_DMA_HEAP);

// 应用层自己申请 dma-buf fd
int fd = -1;
void* ptr = nullptr;
size_t size = frame_size;
dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, size, &fd, &ptr);

MppBufferInfo info = {};
info.type = MPP_BUFFER_TYPE_DMA_HEAP;
info.size = size;
info.fd = fd;
info.ptr = ptr;

mpp_buffer_commit(group, &info);

// 后面从 group 里取 buffer
MppBuffer buffer = nullptr;
mpp_buffer_get(group, &buffer, size);

// 使用 buffer

mpp_buffer_put(buffer);
mpp_buffer_group_put(group);

// 最后应用层释放自己申请的 fd
dma_buf_free(size, &fd, ptr);
```

这种方式适合：

- 应用层想控制真实内存来源
- 需要使用 DMA32
- 需要和 RGA/RKNN/MPP 多模块共享 fd
- 多路编解码需要固定 buffer 池

## 4. Buffer 类型

`mpp_buffer.h` 里定义了这些类型：

```cpp
typedef enum {
    MPP_BUFFER_TYPE_NORMAL,
    MPP_BUFFER_TYPE_ION,
    MPP_BUFFER_TYPE_EXT_DMA,
    MPP_BUFFER_TYPE_DRM,
    MPP_BUFFER_TYPE_DMA_HEAP,
    MPP_BUFFER_TYPE_BUTT,
} MppBufferType;
```

常见类型说明：

| 类型 | 含义 |
| --- | --- |
| `MPP_BUFFER_TYPE_NORMAL` | 普通内存，主要用于测试 |
| `MPP_BUFFER_TYPE_ION` | ION buffer，老内核或 Android 场景较常见 |
| `MPP_BUFFER_TYPE_EXT_DMA` | 应用层传入的通用 dma-buf fd |
| `MPP_BUFFER_TYPE_DRM` | DRM buffer |
| `MPP_BUFFER_TYPE_DMA_HEAP` | Linux DMA_HEAP buffer，较新的内核常用 |

头文件里有一句很重要：

```text
MPP_BUFFER_TYPE_EXT_DMA is only used for general external dma_buf fd import.
```

也就是说，`EXT_DMA` 不是让 MPP 帮你分配内存，而是告诉 MPP：

```text
这是一块外部来的通用 dma-buf fd
```

如果你的 fd 是从 `dma_heap` 申请的，也可以使用 `MPP_BUFFER_TYPE_DMA_HEAP`。

## 5. Buffer flags

头文件里还定义了一些 flags：

```cpp
#define MPP_BUFFER_FLAGS_CONTIG
#define MPP_BUFFER_FLAGS_CACHABLE
#define MPP_BUFFER_FLAGS_WC
#define MPP_BUFFER_FLAGS_SECURE
#define MPP_BUFFER_FLAGS_ALLOC_KMAP
#define MPP_BUFFER_FLAGS_DMA32
```

比较重要的是：

```cpp
MPP_BUFFER_FLAGS_DMA32
```

它表示希望 buffer 位于 DMA32 地址范围内。对于某些 RGA 场景，这很重要，因为你之前遇到过：

```text
RGA_MMU unsupported memory larger than 4G
```

这类问题通常和硬件模块访问 4G 以上物理地址有关。

理论上可以尝试：

```cpp
MPP_BUFFER_TYPE_DMA_HEAP | MPP_BUFFER_FLAGS_DMA32
```

但实际是否生效要看当前 MPP、内核、allocator 支持情况。

## 6. MppBufferInfo

`MppBufferInfo` 用于描述一块要导入或提交给 MPP 的外部 buffer。

定义：

```cpp
typedef struct MppBufferInfo_t {
    MppBufferType type;
    size_t        size;
    void          *ptr;
    void          *hnd;
    int           fd;
    int           index;
} MppBufferInfo;
```

常用字段：

| 字段 | 作用 |
| --- | --- |
| `type` | buffer 类型，比如 `MPP_BUFFER_TYPE_DMA_HEAP` |
| `size` | buffer 大小 |
| `ptr` | CPU 虚拟地址，可选 |
| `fd` | dma-buf fd，硬件共享内存最常用 |
| `index` | buffer 索引，可用于跟踪 |

在 DMA buffer 场景里，最关键的是：

```cpp
info.type = MPP_BUFFER_TYPE_DMA_HEAP;
info.size = frame_size;
info.fd = dma_fd;
info.ptr = virtual_addr;
```

## 7. commit 和 import 的区别

这是 `mpp_buffer.h` 里最容易混淆的地方。

### 7.1 mpp_buffer_commit

宏定义：

```cpp
#define mpp_buffer_commit(group, info) \
    mpp_buffer_import_with_tag(group, info, NULL, MODULE_TAG, __FUNCTION__)
```

特点：

- 必须传入 `group`
- 不直接返回 `MppBuffer`
- 把外部 buffer 提交进 group
- 提交后 buffer 初始状态是 unused
- 后续通过 `mpp_buffer_get(group, &buffer, size)` 取出来使用

适合：

```text
固定 buffer pool
多帧循环使用
解码器外部帧池
编码器输入帧池
```

### 7.2 mpp_buffer_import

宏定义：

```cpp
#define mpp_buffer_import(buffer, info) \
    mpp_buffer_import_with_tag(NULL, info, buffer, MODULE_TAG, __FUNCTION__)
```

特点：

- 不挂到你指定的 group
- 直接返回一个 `MppBuffer`
- 更像一次性导入
- MPP 会把它挂到默认 misc group 里用于追踪

适合：

```text
临时图像处理
单次使用
不需要 pool 管理
```

### 7.3 简单对比

| API | 是否指定 group | 是否直接返回 MppBuffer | 适合场景 |
| --- | --- | --- | --- |
| `mpp_buffer_commit` | 是 | 否 | 固定 buffer 池 |
| `mpp_buffer_import` | 否 | 是 | 临时导入、一次性使用 |

## 8. get / put / inc_ref

### 8.1 mpp_buffer_get

```cpp
mpp_buffer_get(group, &buffer, size);
```

作用：

```text
从 group 中取一块可用 buffer
```

如果 group 是 internal 模式，MPP 可能会自己分配新 buffer。

如果 group 是 external commit 模式，MPP 会从已经 commit 的外部 buffer 中找 unused buffer。

### 8.2 mpp_buffer_put

```cpp
mpp_buffer_put(buffer);
```

作用：

```text
释放当前持有者对 buffer 的引用
```

注意：`put` 不一定等于释放底层内存。只有引用计数降到 0，buffer 才会回到 unused 状态或被释放。

### 8.3 mpp_buffer_inc_ref

```cpp
mpp_buffer_inc_ref(buffer);
```

作用：

```text
增加引用计数
```

常见用途：

- 某个 buffer 要跨线程传递
- 解码输出帧要交给推理线程继续使用
- 一个模块还没用完，不能让原持有者释放

你的项目里 decoder 输出帧给推理线程时，使用 `mpp_buffer_inc_ref()` 是合理的。

## 9. group limit

API：

```cpp
mpp_buffer_group_limit_config(group, size, count);
```

作用：

```text
限制 group 中 buffer 的最大 size 和最大 count
```

头文件注释里说：

```text
limit mode normally use with commit flow and is used for frame buffer
```

也就是说，对于视频帧这种固定大小 buffer，建议配置 limit。

示例：

```cpp
mpp_buffer_group_limit_config(group, frame_size, 8);
```

含义：

```text
这个 group 最多管理 8 块 frame_size 大小的 buffer
```

## 10. fd / ptr / size 查询

常用 API：

```cpp
int fd = mpp_buffer_get_fd(buffer);
void* ptr = mpp_buffer_get_ptr(buffer);
size_t size = mpp_buffer_get_size(buffer);
```

在 RGA 场景里，最常用的是 fd：

```cpp
int dst_fd = mpp_buffer_get_fd(buffer);
rga_buffer_t dst = wrapbuffer_fd(dst_fd, width, height, format, wstride, hstride);
```

这样 RGA 就能通过 fd 操作这块 MPP buffer。

## 11. cache sync

头文件里有这些 API：

```cpp
mpp_buffer_sync_begin(buffer);
mpp_buffer_sync_end(buffer);
mpp_buffer_sync_ro_begin(buffer);
mpp_buffer_sync_ro_end(buffer);
```

它们用于 CPU cache 同步。

如果是 CPU 直接读写 buffer，需要考虑 cache 一致性。

但如果流程是：

```text
RGA 写 DMA buffer
MPP 编码器读 DMA buffer
```

中间没有 CPU 直接写像素，一般不需要你手动做 CPU cache sync。

如果你使用 `mpp_buffer_get_ptr()` 后 CPU 直接写数据，就要小心 cache 同步问题。

## 12. 在编码器中的典型用法

适合你项目当前方向的编码输入池流程是：

```text
初始化：
1. 创建 external MppBufferGroup
2. 用 DMA32 heap 分配多个 dma-buf fd
3. 把 fd commit 到 group

每帧：
1. mpp_buffer_get(group, &buffer, frame_size)
2. 取 fd: mpp_buffer_get_fd(buffer)
3. RGA 写入这个 fd
4. mpp_frame_set_buffer(frame, buffer)
5. encode_put_frame(ctx, frame)
6. 在合适时机 mpp_buffer_put(buffer)

销毁：
1. 停止编码器
2. put group
3. 释放外部 dma-buf fd
```

代码骨架：

```cpp
MppBufferGroup group = nullptr;
mpp_buffer_group_get_external(&group, MPP_BUFFER_TYPE_DMA_HEAP);
mpp_buffer_group_limit_config(group, frame_size, buffer_count);

for (int i = 0; i < buffer_count; ++i) {
    int fd = -1;
    void* ptr = nullptr;
    dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, frame_size, &fd, &ptr);

    MppBufferInfo info = {};
    info.type = MPP_BUFFER_TYPE_DMA_HEAP;
    info.size = frame_size;
    info.fd = fd;
    info.ptr = ptr;

    mpp_buffer_commit(group, &info);
}
```

每帧取 buffer：

```cpp
MppBuffer buffer = nullptr;
MPP_RET ret = mpp_buffer_get(group, &buffer, frame_size);
if (ret != MPP_OK || buffer == nullptr) {
    // 没有空闲 buffer，可以丢帧或等待
}
```

送给编码器：

```cpp
MppFrame frame = nullptr;
mpp_frame_init(&frame);
mpp_frame_set_width(frame, width);
mpp_frame_set_height(frame, height);
mpp_frame_set_hor_stride(frame, hor_stride);
mpp_frame_set_ver_stride(frame, ver_stride);
mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
mpp_frame_set_buffer(frame, buffer);

mpi->encode_put_frame(ctx, frame);

mpp_frame_deinit(&frame);
```

## 13. put 的时机要小心

`mpp_buffer_put(buffer)` 表示释放应用层对这块 buffer 的引用。

但对于硬件编码器来说，有一个非常重要的问题：

```text
encode_put_frame() 返回，不一定代表硬件已经读完 input buffer
```

所以如果 simple API 没有帮你稳定持有内部引用，过早 `mpp_buffer_put()` 可能导致这块 buffer 很快被再次取出，然后被 RGA 写下一帧，引发：

```text
花屏
画面重叠
马赛克
帧混在一起
```

更稳的做法是：

```text
mpp_buffer_get()
RGA 写
encode_put_frame()
buffer 放入 inflight 队列
encode_get_packet() 收到对应输出后
mpp_buffer_put(buffer)
```

也就是：

```text
MppBufferGroup 管理 buffer pool
应用层 inflight 队列管理“编码器正在使用”的业务状态
```

如果使用 `MppTask` 模式，输入和输出对应关系会更清晰，更适合严格管理生命周期。

## 14. 在解码器中的典型用法

解码器外部 buffer 池常见流程：

```text
1. 应用层创建 external group
2. 分配一批 dma-buf fd
3. commit 到 group
4. 通过 MPP_DEC_SET_EXT_BUF_GROUP 之类接口交给解码器
5. 解码器输出 MppFrame
6. 应用层从 MppFrame 里拿 MppBuffer / fd
```

你的项目里 decoder 已经接近这种模式：

```cpp
mpp_buffer_group_get_external(&m_frm_grp, MPP_BUFFER_TYPE_DMA_HEAP);
mpp_buffer_commit(m_frm_grp, &commit_info);
```

这样解码器输出帧就是外部 DMA buffer，后续可以把 fd 交给 RGA/RKNN。

## 15. 结合你项目的理解

你项目现在有几类 buffer：

```text
解码器输出 buffer:
  MPP decoder 使用
  RGA/RKNN 读取

RKNN 输入 buffer:
  RGA 写入 RGB
  RKNN 读取

编码器输入 buffer:
  RGA 写入 NV12 + 画框
  MPP encoder 读取

编码器输出 packet:
  MPP encoder 输出 H264
  FFmpeg RTSP publisher 推流
```

其中 `MppBufferGroup` 最适合管理：

```text
解码器输出帧池
编码器输入帧池
```

但它不能完全代替业务层状态管理。尤其是编码器 simple API 下，你仍然需要清楚：

```text
这块 input buffer 是否还可能被硬件编码器读取
```

## 16. 常见坑

### 16.1 import 和 commit 混用

`mpp_buffer_import()` 是直接生成一个 `MppBuffer`。

`mpp_buffer_commit()` 是把外部 buffer 放进 group。

如果你想做 buffer pool，优先用 `commit + get + put`。

### 16.2 fd 类型和 group 类型不一致

如果 fd 来自 DMA_HEAP，建议先使用：

```cpp
MPP_BUFFER_TYPE_DMA_HEAP
```

如果是一般外部 dma-buf fd，可以尝试：

```cpp
MPP_BUFFER_TYPE_EXT_DMA
```

但 group type 和 `MppBufferInfo.type` 最好保持一致。

### 16.3 过早复用编码输入 buffer

这是花屏、马赛克、画面重叠的常见原因。

错误思路：

```text
encode_put_frame()
马上认为 buffer 可复用
```

更稳思路：

```text
encode_get_packet() 收到输出后
再回收对应输入 buffer
```

### 16.4 buffer 数量太少

编码器、RGA、推理、RTSP 都是异步链路。buffer 数量太少会导致：

```text
mpp_buffer_get 失败
丢帧
卡顿
```

多路视频时，编码输入 buffer 数量通常需要比 4 更宽裕，例如 8 或 12。

### 16.5 CPU 直接访问 DMA buffer 时忘记 cache sync

如果只用 RGA/MPP/RKNN 通过 fd 访问，通常问题不大。

如果 CPU 通过 ptr 读写像素，需要考虑：

```cpp
mpp_buffer_sync_begin(buffer);
mpp_buffer_sync_end(buffer);
```

## 17. 推荐路线

对于你当前项目，建议理解成三层：

```text
第一层：真实内存来源
DMA32 heap，避免 RGA 4G 地址问题

第二层：buffer pool
MppBufferGroup external commit/get/put

第三层：业务生命周期
free / inflight / completed 状态
```

如果继续用 simple encoder API，比较稳的工程方案是：

```text
DMA32 fd
-> mpp_buffer_commit 到 MppBufferGroup
-> mpp_buffer_get 取输入 buffer
-> RGA 写
-> encode_put_frame
-> 放入 inflight 队列
-> encode_get_packet 输出后
-> mpp_buffer_put 回 group
```

如果未来追求更行业化，可以进一步切到：

```text
MppTask 模式
```

`MppTask` 能更清楚地表达输入帧、输出包和任务完成关系，更适合多路、高吞吐、严格生命周期管理的项目。
