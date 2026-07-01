# 项目线程关系与队列缓存梳理

这份文档只关注当前项目里“线程之间怎么交接数据”，以及交接点上有哪些队列/缓存。大的 DMA 图像内存池不是本文重点，只有在它会影响线程阻塞或帧释放时才会提到。

## 总览图

线程拓扑如下：

```text
┌────────────────────────────────────────────────────────────────────┐
│                              main.cc                               │
│                                                                    │
│  主线程做两件关键事：                                                │
│    1. 创建并启动所有模块                                             │
│    2. 循环 pipeline.TryGet()，把模型结果送回对应 VideoChannel         │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ pipeline.TryGet()
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                     MultiModelPipeline / rknnPool                  │
│                                                                    │
│  同一帧 FrameContext                                                │
│       │                                                            │
│       ├──> yolo adapter      -> yolo rknnPool      -> RKNN workers  │
│       │                         │                                  │
│       │                         ├─ ThreadPool::tasks_               │
│       │                         └─ completed_outputs                │
│       │                                                            │
│       └──> face_yolo adapter -> face_yolo rknnPool -> RKNN workers  │
│                                 │                                  │
│                                 ├─ ThreadPool::tasks_               │
│                                 └─ completed_outputs                │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ main 取出 ModelOutput 后按 channel_id 分发
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                         4 路 VideoChannel                          │
│                                                                    │
│  每一路都有自己的线程和队列：                                         │
│                                                                    │
│  ┌──────────────────────────────┐                                  │
│  │ m_decode_thread              │                                  │
│  │   DecodeLoop()               │                                  │
│  │     -> fread H264 packet      │                                  │
│  │     -> MppDecoder             │                                  │
│  │     -> 解码回调               │                                  │
│  │     -> pipeline.Submit(frame) │                                  │
│  └──────────────────────────────┘                                  │
│                                                                    │
│  ┌──────────────────────────────┐                                  │
│  │ m_model_output_queue         │ <── main: OnModelOutput()         │
│  │ ThreadSafeQueue<ModelOutput> │                                  │
│  └──────────────┬───────────────┘                                  │
│                 ▼                                                  │
│  ┌──────────────────────────────┐                                  │
│  │ m_output_thread              │                                  │
│  │   OutputLoop()               │                                  │
│  │     -> ProcessModelOutput()   │                                  │
│  │     -> m_model_reorder_buffer │                                  │
│  │     -> aggregator.Submit()    │                                  │
│  └──────────────────────────────┘                                  │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ 每路按 frame_id 重排后提交
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                       FrameResultAggregator                        │
│                                                                    │
│  worker_ 聚合线程                                                   │
│                                                                    │
│  queue_                                                            │
│    暂存各通道送来的 ModelOutput                                     │
│                                                                    │
│  pending_                                                          │
│    key = channel_id + frame_id                                      │
│    同一帧多个模型结果在这里合并                                      │
│                                                                    │
│  expected_publish_frame_                                            │
│    控制每一路按 frame_id 顺序发布                                    │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  │ ComposedFrame
                                  │   -> channels[ch]->OnFrameAggregated()
                                  │   -> mosaic.Submit(frame)
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                          MosaicComposer                            │
│                                                                    │
│  latest_[0..3]                                                      │
│    每路保存一帧最新 ComposedFrame                                   │
│                                                                    │
│  flow_thread_                                                       │
│    固定 fps tick                                                    │
│    -> 从 latest_ 取四路最新帧                                       │
│    -> RGA 合成四宫格                                                │
│    -> encoder.PushBuffer()                                          │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                            RkMppEncoder                            │
│                                                                    │
│  pending_frames_                                                    │
│    已经送进编码器、但还没确认编码完成的输入帧                         │
│                                                                    │
│  output_thread_                                                     │
│    encode_get_packet()                                              │
│    -> on_packet_ready_                                              │
│    -> RtspPublisher::Push()                                         │
│    -> 回收 pending_frames_ 中对应输入帧                              │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                         RtspPublisher                              │
│                                                                    │
│  FFmpeg av_interleaved_write_frame() 写 RTSP                        │
└────────────────────────────────────────────────────────────────────┘
```

一帧数据的流转可以再压缩成下面这张图：

```text
本地 H264 文件
    │
    ▼
VideoChannel::m_decode_thread
    │
    ├─ DecodeLoop()
    │     │
    │     ▼
    │   MppDecoder::DecodePacket()
    │     │
    │     ▼
    │   MppDecoder::FlushDecoder()
    │     │
    │     ▼
    │   解码回调 lambda
    │     │
    │     ├─ 生成 FrameContext
    │     ├─ m_inflight_frames++
    │     └─ MultiModelPipeline::Submit()
    │
    ▼
MultiModelPipeline
    │
    ├─ yolo      -> rknnPool::put() -> ThreadPool::tasks_ -> RKNN worker
    │                                      │
    │                                      ▼
    │                                completed_outputs
    │
    └─ face_yolo -> rknnPool::put() -> ThreadPool::tasks_ -> RKNN worker
                                           │
                                           ▼
                                     completed_outputs

main 主线程
    │
    ├─ pipeline.TryGet()
    │
    └─ channels[channel_id]->OnModelOutput(output)
          │
          ▼
VideoChannel::m_model_output_queue
    │
    ▼
VideoChannel::m_output_thread
    │
    ├─ OutputLoop()
    ├─ ProcessModelOutput()
    ├─ m_model_reorder_buffer 按 frame_id 重排
    └─ FrameResultAggregator::Submit()
          │
          ▼
FrameResultAggregator::queue_
    │
    ▼
FrameResultAggregator::worker_
    │
    ├─ pending_ 合并同一帧的多个模型结果
    ├─ expected_publish_frame_ 保证每路顺序
    └─ 输出 ComposedFrame
          │
          ├─ VideoChannel::OnFrameAggregated()
          │     └─ m_inflight_frames--
          │
          └─ MosaicComposer::Submit()
                │
                ▼
MosaicComposer::latest_[channel_id]
    │
    ▼
MosaicComposer::flow_thread_
    │
    ├─ 固定 fps tick
    ├─ RGA 四宫格合成
    └─ RkMppEncoder::PushBuffer()
          │
          ▼
RkMppEncoder::pending_frames_
    │
    ▼
RkMppEncoder::output_thread_
    │
    ├─ encode_get_packet()
    ├─ RtspPublisher::Push()
    └─ 回收编码输入帧
```

## 当前有哪些线程

### 主线程

位置：`src/main.cc`

主线程不是单纯等待退出，它现在是一个“模型结果分发线程”：

- 创建两个 `rknnPool`：`yolo` 和 `face_yolo`。
- 创建 4 个 `VideoChannel`。
- 启动 `FrameResultAggregator::worker_`。
- 启动 `MosaicComposer::flow_thread_`。
- 循环调用 `pipeline.TryGet(output)`，从各模型池取已经完成的 `ModelOutput`。
- 根据 `output.frame.channel_id` 把结果送回对应的 `VideoChannel::OnModelOutput()`。

主线程这里的交接点是：模型池的 `completed_outputs` -> 对应通道的 `m_model_output_queue`。

### 每路解码线程

位置：`src/StreamChannel.cpp`

每个 `VideoChannel::start()` 会创建一个：

```cpp
m_decode_thread = std::thread(&VideoChannel::DecodeLoop, this);
```

这个线程负责：

- 从本地 H264 文件 `fread()` 读码流。
- 调用 `m_decoder->DecodePacket(buffer, bytes_read)`。
- `MppDecoder::FlushDecoder()` 拿到 MPP 解码后的 `MppFrame`。
- 在解码回调里构造 `FrameContext`。
- 调用 `m_pipeline->Submit(task_frame)` 把同一帧分发给所有模型。

注意：解码回调是在当前解码线程里同步执行的，不是额外的新线程。也就是说，回调里 `sleep_for()`、等待 `m_inflight_frames`、等待 `pipeline.PendingCount()`，都会直接让这一通道的解码线程变慢。

### 每路模型输出线程

位置：`src/StreamChannel.cpp`

每个 `VideoChannel::start()` 还会创建一个：

```cpp
m_output_thread = std::thread(&VideoChannel::OutputLoop, this);
```

这个线程负责：

- 从 `m_model_output_queue` 里取 `ModelOutput`。
- 调用 `ProcessModelOutput()`。
- 按 `frame_id` 做本通道重排。
- 顺序正确后，把结果提交给 `FrameResultAggregator`。

它解决的是：RKNN 多线程推理返回顺序不稳定，所以每路需要先恢复视频帧顺序。

### 每个模型池的 RKNN worker 线程

位置：`include/rknnPool.hpp`、`include/ThreadPool.hpp`

每个 `rknnPool` 内部有一个 `ThreadPool`：

```cpp
pool = std::make_unique<dpool::ThreadPool>(threadNum);
```

每次 `pool_->put(data)` 会：

- 选择一个 RKNN model/context。
- `pending_count++`。
- 把一次 `model->infer(inputData)` 封装成任务放进 `ThreadPool::tasks_`。
- worker 线程执行 RGA 预处理、RKNN 推理、后处理。
- 推理完成后把 `InferOutput` 放进 `completed_outputs`。

现在有两个模型池：

- `yolo` 模型池。
- `face_yolo` 模型池。

它们各自有自己的 worker 线程、任务队列、完成队列。

### 聚合器线程

位置：`src/FrameResultAggregator.cpp`

`aggregator.Start()` 创建：

```cpp
worker_ = std::thread(&FrameResultAggregator::WorkerLoop, this);
```

这个线程负责：

- 从 `queue_` 取各模型结果。
- 用 `(channel_id, frame_id)` 合并同一帧的多个模型结果。
- 等 required models 都回来，或者超时。
- 按每个通道的 `expected_publish_frame_` 顺序发布 `ComposedFrame`。
- 发布后调用回调：先 `VideoChannel::OnFrameAggregated()` 释放 inflight，再 `mosaic.Submit(frame)`。

它解决的是：多模型结果不是一次性出来的，同一帧需要等多个模型结果拼齐。

### 马赛克合成线程

位置：`src/MosaicComposer.cpp`

`mosaic.Init()` 创建：

```cpp
flow_thread_ = std::thread(&MosaicComposer::FlowLoop, this);
```

这个线程负责：

- 按固定 fps tick，比如 24fps。
- 检查 `latest_[4]` 四路是否都有有效帧。
- 从 `mosaic_grp_` 取一块输出 buffer。
- 用 RGA 把四路最新帧拼成四宫格。
- 画多模型结果。
- 调用 `encoder_->PushBuffer(dst_buffer)` 送编码。

它不是“来一帧合成一帧”，而是固定节奏取当前每路最新帧合成。所以 `latest_[4]` 是它和聚合器之间最重要的缓存。

### 编码器输出线程

位置：`src/MppEncoder.cpp`

`encoder_->Start()` 创建：

```cpp
output_thread_ = std::thread(&RkMppEncoder::OutputThreadFunc, this);
```

这个线程负责：

- 调用 `encode_get_packet()` 从 MPP 硬件编码器取码流包。
- 触发 `on_packet_ready_` 回调。
- 回调里交给 `RtspPublisher::Push()`。
- 判断一帧编码完成后，从 `pending_frames_` 回收最老的输入帧。

这里的 `pending_frames_` 很关键：它不是模型重排队列，而是“已经送进编码器，但还没确认编码完成”的输入帧队列。

## 线程之间的队列/缓存

| 位置 | 名称 | 生产者 | 消费者 | 作用 |
| --- | --- | --- | --- | --- |
| `ThreadPool` | `tasks_` | `rknnPool::put()` | RKNN worker | 推理任务队列 |
| `rknnPool` | `completed_outputs` | RKNN worker | 主线程 `pipeline.TryGet()` | 推理完成结果队列 |
| `VideoChannel` | `m_model_output_queue` | 主线程 `OnModelOutput()` | 每路 `OutputLoop()` | 把模型结果交回对应通道 |
| `VideoChannel` | `m_model_reorder_buffer` | `ProcessModelOutput()` | `ProcessModelOutput()` 自己按顺序弹出 | 每路按 `frame_id` 重排 |
| `FrameResultAggregator` | `queue_` | 每路 `OutputLoop()` | 聚合器 `worker_` | 多模型结果输入队列 |
| `FrameResultAggregator` | `pending_` | 聚合器 `AddResult()` | 聚合器 `PublishAvailableLocked()` | 同一帧等待多个模型结果 |
| `FrameResultAggregator` | `expected_publish_frame_` | 聚合器 | 聚合器 | 每路发布顺序门控 |
| `MosaicComposer` | `latest_[4]` | 聚合器回调 `mosaic.Submit()` | 马赛克 `flow_thread_` | 每路最新可合成帧 |
| `MosaicComposer` | `mosaic_grp_` | `AllocateOutputBuffers()` | `ComposeLocked()`/编码器 | 四宫格输出 DMA buffer 池 |
| `RkMppEncoder` | `pending_frames_` | `PushBuffer()` | `OutputThreadFunc()` | 编码输入帧等待回收 |

还有一个旧路径队列：

| 位置 | 名称 | 当前状态 |
| --- | --- | --- |
| `VideoChannel` | `m_output_queue<InferOutput>` | 旧单模型路径使用，现在多模型主链路主要走 `m_model_output_queue<ModelOutput>` |

## 一帧的跨线程流转

1. `m_decode_thread` 读 H264 packet。
2. `MppDecoder` 解出 `MppFrame`，在同一个解码线程触发回调。
3. 回调里拿到 `src_fd` 和 `MppBuffer`，给 `MppBuffer` 加引用，生成 `FrameContext`。
4. `MultiModelPipeline::Submit()` 把同一帧复制给每个模型。
5. 每个模型提交前再 `mpp_buffer_inc_ref()`，保证不同模型异步使用同一张解码图时，底层 buffer 不会提前释放。
6. `YoloModelAdapter::Submit()` 把 `FrameContext` 转成 `input_data`，放进对应 `rknnPool`。
7. RKNN worker 线程完成推理，把结果放进模型池 `completed_outputs`。
8. 主线程 `pipeline.TryGet()` 轮询所有模型池，取到一个 `ModelOutput`。
9. 主线程根据 `channel_id` 调用对应通道 `OnModelOutput()`，把结果放进 `m_model_output_queue`。
10. 每路 `m_output_thread` 取出结果，按 `frame_id` 做重排。
11. 重排顺序满足后，结果进入 `FrameResultAggregator::queue_`。
12. 聚合器线程按 `(channel_id, frame_id)` 把多模型结果合并到 `pending_`。
13. required models 到齐，或者等待超过 timeout 后，聚合器发布 `ComposedFrame`。
14. 聚合器回调里调用 `OnFrameAggregated()`，减少本通道 `m_inflight_frames`。
15. 同一个回调再调用 `mosaic.Submit(frame)`，把这一帧更新到 `latest_[channel_id]`。
16. 马赛克线程按固定 fps 从 `latest_[4]` 取四路最新帧，用 RGA 合成四宫格。
17. 合成帧送入 `RkMppEncoder::PushBuffer()`，进入 `pending_frames_`。
18. 编码器输出线程拿到 H264 packet 后，调用 `RtspPublisher::Push()` 推 RTSP，并回收对应输入帧。

## 当前的反压位置

### 每路 inflight 限制

位置：`VideoChannel::DecodeLoop()` 和解码回调。

```cpp
if (m_inflight_frames.load() < max_inflight) {
    break;
}
std::this_thread::sleep_for(std::chrono::milliseconds(2));
```

含义：如果这一通道已经有太多帧在后面模型/聚合/马赛克链路里，就暂停继续读包和解码。

### 全局模型池 pending 限制

位置：`VideoChannel::DecodeLoop()`。

```cpp
while (m_running && m_pipeline && m_pipeline->PendingCount() >= 40) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
```

含义：如果所有模型池的待完成任务太多，就让本地读取线程睡眠，避免继续把解码帧塞进系统。

### 本地文件输入节流

位置：解码回调。

本地文件不是实时源，读取会远快于真实 fps。代码里按 `m_input_fps` 计算目标时间，必要时 `sleep_until()`，让本地文件更像实时输入。

### 重排超时

位置：`VideoChannel::ProcessModelOutput()`。

如果某一路某个 `frame_id` 一直不回来，超过 `m_reorder_timeout` 后：

- 记录 `model_reorder_timeout`。
- 调用 `m_aggregator->SkipFrame()`。
- 调用 `OnFrameAggregated()` 释放 inflight。
- `m_expected_frame_id++`，继续向后走。

这个是防止单个丢失帧卡死整路视频顺序的关键。

### 聚合超时

位置：`FrameResultAggregator::PublishAvailableLocked()`。

如果同一帧的 required models 没有全部回来，但等待超过 `timeout_`，也会发布 partial `ComposedFrame`。这样可以避免某个模型慢或异常时，视频完全卡住。

## 最容易卡住的位置

1. 解码 buffer 耗尽：后面链路长期持有 `MppBuffer`，`m_inflight_frames` 不下降，解码器 24 块外部 buffer 被占满。
2. 模型池堆积：`pending_count` 一直上升，`pipeline.PendingCount() >= 40` 触发，解码线程被反压睡眠。
3. 重排等待：某个 `frame_id` 没回来，`m_model_reorder_buffer` 里后续帧都不能立刻发布。
4. 聚合等待：同一帧需要的多个模型没到齐，`pending_` 里等待 timeout 或全部到齐。
5. 马赛克输出 buffer 忙：`mpp_buffer_get(mosaic_grp_)` 失败时会丢一帧 mosaic。
6. 编码器 pending 未回收：`pending_frames_` 堆积，说明送入编码的帧多于编码输出回收速度。

## 简化理解

可以把现在的项目理解成 6 层异步流水线：

```text
每路解码线程
    -> 多模型 RKNN 线程池
    -> 主线程取模型结果
    -> 每路输出线程做重排
    -> 聚合器线程做多模型同步
    -> 马赛克线程按 fps 合成
    -> 编码器输出线程推流
```

其中真正跨线程保存数据的核心队列是：

```text
ThreadPool::tasks_
rknnPool::completed_outputs
VideoChannel::m_model_output_queue
VideoChannel::m_model_reorder_buffer
FrameResultAggregator::queue_
FrameResultAggregator::pending_
MosaicComposer::latest_
RkMppEncoder::pending_frames_
```

排查问题时可以优先看这些点的大小、增长趋势和释放时机。
