# 多路实时推流线程池架构学习笔记

## 1. 当前项目的核心流程

当前项目大致流程是：

```text
H264 文件
  -> MPP 解码
  -> 解码帧投递到全局 RKNN 线程池
  -> 主线程从 RKNN 线程池取推理结果
  -> 根据 channel_id 分发回对应 VideoChannel
  -> 每路内部按 frame_id 重排
  -> RGA 拷贝 / 画框
  -> MPP 编码
  -> RTSP 推流到 ZLMediaKit
  -> VLC 拉流观看
```

两路视频时，当前结构可以理解为：

```text
channel0 decode \
                 -> shared rknnPool -> main thread -> channel output/encode/push
channel1 decode /
```

多路视频共用一个推理线程池本身是合理的。行业里多路视频共享一个 NPU/GPU 推理资源池也很常见，关键问题不在于“能不能一起放进去”，而在于“结果如何取出、如何分发、慢帧如何处理”。

## 2. 当前卡顿现象

这次日志里，0 路在播放到约 1 分 35 秒到 1 分 40 秒附近出现：

```text
画面卡住
  -> 随后突然往前赶
  -> 可能伴随花屏 / 马赛克 / 快进感
```

从日志看，网络推流本身不是主要原因。因为卡顿段里 `push_us` 很小，通常只有几百微秒级，说明 RTSP 写包不是瓶颈。

真正的问题更靠前：0 路部分帧已经完成 RKNN 推理，但没有及时进入 `reorder -> encode -> packet_push`。

典型日志现象：

```text
frame 2314 rknn_infer 已完成
  -> 过了几百毫秒才 reorder_pop

frame 2316 rknn_infer 已完成
  -> 过了将近 1 秒才 reorder_pop

随后 packet_push 出现 300ms / 1000ms / 621ms / 1135ms 级别间隔
之后又开始几毫秒一帧地连续吐包
```

这说明卡顿不是单纯编码器输出慢，而是上游结果消费和通道重排被阻塞后，积压帧又被集中释放。

## 3. 为什么大部分时间正常，偶尔会卡

平时各环节耗时接近实时要求：

```text
RKNN 推理：约 20ms - 50ms
编码/推流节奏：24fps，约 41.6ms 一帧
```

如果每一帧都比较均匀，系统可以正常播放。

但多路实时系统里，只要某一帧或某一路偶尔慢一下，就可能触发连锁反应：

```text
某个 future 比较慢
  -> 主线程等待队首结果
  -> 后面已经完成的结果也取不出来
  -> 某一路的 expected_frame_id 等不到
  -> reorder_buffer 堆积
  -> 编码器一段时间没帧
  -> VLC 画面卡住
  -> 等慢帧回来后，积压帧集中输出
  -> VLC 看到突然往前放
```

所以它不是一直卡，而是偶发卡顿。原因是系统平均性能可能够，但实时管线没有足够好的“慢帧处理策略”。

## 4. 当前线程池的关键问题

当前 `rknnPool::get()` 是按提交顺序取结果：

```cpp
outputData = futs.front().get();
futs.pop();
```

这意味着：

```text
提交顺序：
0路 frame100
1路 frame100
0路 frame101
1路 frame101

如果队首 0路 frame100 很慢：
即使后面的 1路 frame100 / 0路 frame101 已经完成，
主线程也无法越过队首去取它们。
```

这叫 FIFO 队首阻塞。

对单路视频影响可能不明显，因为顺序本来就接近一致。对多路视频影响会放大，因为不同路、不同帧的耗时不同，任何一个慢任务都可能挡住后面的已完成任务。

## 5. 多路视频能不能共用线程池

可以。

更准确地说：

```text
多路视频可以共用推理线程池。
但线程池结果不应该按提交顺序阻塞式取出。
应该按完成顺序进入 completed_queue。
```

推荐结构：

```text
channel0 frame -> submit \
channel1 frame -> submit  -> RKNN workers
channel2 frame -> submit /

worker 谁先完成：
  -> push 到 completed_queue

主线程 / 调度线程：
  -> 从 completed_queue 取已完成结果
  -> 根据 channel_id 分发回对应通道
```

这样慢帧只影响它自己，不会阻塞后面已经完成的其他帧。

## 6. 当前主线程同步分发的问题

当前主线程逻辑大致是：

```cpp
if (testPool.get(out) == 0) {
    channels[out.channel_id]->OnInferOutput(out);
}
```

而 `OnInferOutput()` 里面继续做：

```text
reorder
  -> EncodeZeroCopy
  -> RGA copy/draw
  -> PushBuffer 到编码器
```

这会导致主线程不只是“分发结果”，还参与了每一路的输出处理。

如果 1 路 `OnInferOutput()` 或编码入口耗时较久，0 路已经完成的推理结果也无法及时被主线程处理。

更合理的做法是：

```text
主线程只做快速分发：
  completed result -> channel result queue

每个 channel 自己有 output thread：
  channel result queue -> reorder -> encode -> push
```

这样 0 路和 1 路输出阶段互不阻塞。

## 7. Reorder 的队首等待问题

每路内部为了保证画面顺序，会按 `frame_id` 等待：

```text
expected_frame_id = 2314

如果 frame 2314 没有进入 reorder：
  frame 2315
  frame 2316
  frame 2317
即使已经完成，也不能直接输出。
```

这对保证顺序有帮助，但实时视频不能无限等。

行业里的实时预览/推流通常会设置超时策略：

```text
如果 expected_frame_id 等待超过 2-3 帧周期：
  -> 丢掉这个缺失帧
  -> expected_frame_id++
  -> 继续输出后面的帧
```

24fps 下，一帧约 41.6ms，可以先尝试：

```text
reorder_timeout = 80ms - 120ms
```

这样最多丢一两帧，画面会小跳一下，但不会卡 1-3 秒。

## 8. 行业里常见的实时视频策略

成熟视频框架一般不会无限排队等待历史帧，而是使用：

```text
有界队列
阶段解耦
完成队列
超时丢帧
QoS 统计
实时链路和录像链路分离
```

### GStreamer queue

GStreamer 的 `queue` 会把前后级解耦到不同线程。队列默认满了会阻塞上游，但可以配置 `leaky`，在满的时候丢新帧或旧帧。

这对应到当前项目，就是不要让某一级无限积压，也不要让某一级慢了以后拖死整条链路。

参考：

https://gstreamer.freedesktop.org/documentation/coreelements/queue.html

### GStreamer appsink

`appsink` 文档也提到，如果应用拉取样本不够快，内部队列会占用大量内存。因此需要 `max-buffers / max-time / leaky-type` 这类参数控制队列。

这对应当前项目里的 RKNN 结果消费和通道输出队列。

参考：

https://gstreamer.freedesktop.org/documentation/app/appsink.html

### GStreamer QoS

GStreamer QoS 的核心思想是：实时管线要测量 lateness，如果 buffer 已经太晚，就应该上游降速、丢帧、降低质量，而不是继续等待。

参考：

https://gstreamer.freedesktop.org/documentation/additional/design/qos.html

### NVIDIA DeepStream

DeepStream 的 `nvstreammux` 用于多路流合批。它不会无限等待所有输入都齐，而是有 `batched-push-timeout`：batch 满了就推，没满但超时也推。

这说明多路实时系统里，一个慢源不能无限拖住整个系统。

参考：

https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreammux.html

## 9. 推荐改造路线

### 阶段一：修正 RKNN 线程池结果返回方式

目标：

```text
从 FIFO future 队列
改成 completed_queue
```

当前：

```text
按提交顺序等待结果
```

推荐：

```text
worker 任务完成后主动把 InferOutput push 到 completed_queue
get() 只从 completed_queue 取已经完成的结果
```

收益：

```text
避免队首慢帧阻塞后面已完成结果
多路视频共享线程池更合理
```

### 阶段二：每路增加独立输出线程

目标：

```text
主线程只分发，不做编码入口处理
```

推荐结构：

```text
main thread:
  completed_queue -> channel[i].PushInferResult(out)

channel output thread:
  result_queue -> reorder -> EncodeZeroCopy -> encoder
```

收益：

```text
1 路编码慢不会阻塞 0 路结果分发
0 路和 1 路输出阶段互相独立
```

### 阶段三：reorder 增加超时跳帧

目标：

```text
实时预览链路不要无限等待缺失帧
```

策略：

```text
如果 expected_frame_id 等待超过 80ms - 120ms：
  -> 记录 drop
  -> expected_frame_id++
  -> 继续输出
```

收益：

```text
用少量丢帧换低延迟
避免几秒级卡顿和集中追帧
```

### 阶段四：全链路 QoS 日志

建议长期保留以下统计：

```text
每路 decode fps
每路 rknn submit fps
每路 rknn complete fps
每路 result queue size
每路 reorder buffer size
每路 encode fps
每路 packet push fps
每路 late_ms
每路 dropped frames
```

这样以后看到卡顿，可以快速判断：

```text
是输入断了？
是推理慢了？
是结果没消费？
是 reorder 卡住？
是编码器卡住？
还是 RTSP push 卡住？
```

## 10. 实时预览和录像的区别

实时预览/AI 推流链路：

```text
目标：低延迟
策略：允许丢帧、跳帧、降低质量
```

录像链路：

```text
目标：完整性
策略：尽量不丢帧，允许更大缓冲和延迟
```

安防/车载项目里，这两条链路通常会分开设计。预览链路保实时，录像链路保完整，不要用同一套策略同时满足两个目标。

## 11. 总结

当前项目多路视频共用 RKNN 线程池是可以的，但需要改成更适合实时系统的方式：

```text
可以共用线程池
但不能按提交顺序阻塞取结果

可以按帧号重排
但不能无限等待缺失帧

可以做背压
但必须是有界背压，并配合丢帧策略

可以一个主线程调度
但主线程不应该同步执行每路编码入口
```

最终目标架构：

```text
Decode per channel
  -> bounded infer input queue
  -> shared RKNN worker pool
  -> completed_queue
  -> dispatch by channel_id
  -> per-channel output queue/thread
  -> reorder with timeout
  -> encode
  -> RTSP push
```

这类设计更接近行业里的多路实时视频处理方式。
