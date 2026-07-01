# 项目整体架构说明

本文档整理当前项目的整体架构。当前项目已经形成了一个多路视频输入、多模型推理、多模型结果聚合、四宫格合成、MPP 编码和 RTSP 推流的实时处理链路。

## 1. 总体链路

```text
4 路视频输入
  -> 每路解码
  -> 每帧分发给多个模型池
  -> 多模型结果按 channel_id + frame_id 聚合
  -> 四宫格 compositor 固定帧率合成
  -> MPP 编码
  -> RTSP 推流
```

整体结构如下：

```text
┌────────────────────────────────────────────────────────────────────┐
│                              main.cc                               │
│                                                                    │
│  创建 2 个 RKNN 池：yoloPool / facePool                             │
│  创建 2 个 ModelAdapter：yolo / face_yolo                           │
│  创建 MultiModelPipeline                                            │
│  创建 FrameResultAggregator                                         │
│  创建 MosaicComposer                                                │
│  创建 4 个 VideoChannel                                             │
└────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                         4 路 VideoChannel                          │
│                                                                    │
│  ch0: test.h264                                                     │
│  ch1: test2.h264                                                    │
│  ch2: test3.h264                                                    │
│  ch3: test4.h264                                                    │
└────────────────────────────────────────────────────────────────────┘
      │                 │                 │                 │
      ▼                 ▼                 ▼                 ▼

┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ MppDecoder   │  │ MppDecoder   │  │ MppDecoder   │  │ MppDecoder   │
│ 解码出 NV12帧 │  │ 解码出 NV12帧 │  │ 解码出 NV12帧 │  │ 解码出 NV12帧 │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │                 │
       └─────────────────┴────────┬────────┴─────────────────┘
                                  ▼

┌────────────────────────────────────────────────────────────────────┐
│                       MultiModelPipeline                           │
│                                                                    │
│  同一帧 FrameContext                                                │
│      -> yolo adapter                                                │
│      -> face_yolo adapter                                           │
│                                                                    │
│  每个模型分支使用自己的 buffer 引用，避免异步推理时提前释放原始帧      │
└────────────────────────────────────────────────────────────────────┘
                   │                              │
                   ▼                              ▼

        ┌──────────────────┐          ┌──────────────────┐
        │ YoloModelAdapter │          │ YoloModelAdapter │
        │ model_id="yolo"  │          │ model_id="face"  │
        └────────┬─────────┘          └────────┬─────────┘
                 │                             │
                 ▼                             ▼

        ┌──────────────────┐          ┌──────────────────┐
        │ rknnPool YOLO    │          │ rknnPool FACE    │
        │ 3 threads        │          │ 9 threads        │
        └────────┬─────────┘          └────────┬─────────┘
                 │                             │
                 ▼                             ▼

        ┌──────────────────┐          ┌──────────────────┐
        │ ModelOutput      │          │ ModelOutput      │
        │ ch/frame/yolo    │          │ ch/frame/face    │
        └────────┬─────────┘          └────────┬─────────┘
                 └──────────────┬──────────────┘
                                ▼

┌────────────────────────────────────────────────────────────────────┐
│                       FrameResultAggregator                        │
│                                                                    │
│  key = { channel_id, frame_id }                                    │
│                                                                    │
│  pending_[ch0, frame100]                                           │
│      results:                                                      │
│        - yolo result                                               │
│        - face_yolo result                                          │
│                                                                    │
│  yolo 和 face_yolo 都到了 -> 输出完整 ComposedFrame                 │
│  超时没到齐 -> 输出 partial ComposedFrame                           │
└────────────────────────────────────────────────────────────────────┘
                                │
                                ▼

┌────────────────────────────────────────────────────────────────────┐
│                          MosaicComposer                            │
│                                                                    │
│  latest_[0] = ch0 最新 ComposedFrame                                │
│  latest_[1] = ch1 最新 ComposedFrame                                │
│  latest_[2] = ch2 最新 ComposedFrame                                │
│  latest_[3] = ch3 最新 ComposedFrame                                │
│                                                                    │
│  FlowLoop 按固定 fps tick                                           │
│      -> RGA 把 4 路画面缩放贴到 1920x1080 四宫格                     │
│      -> RenderModelResult 画检测框                                  │
└────────────────────────────────────────────────────────────────────┘
                                │
                                ▼

┌────────────────────────────────────────────────────────────────────┐
│                         RkMppEncoder                               │
│                                                                    │
│  输入：合成后的 1920x1080 NV12                                      │
│  输出：H264/H265 packet                                             │
└────────────────────────────────────────────────────────────────────┘
                                │
                                ▼

┌────────────────────────────────────────────────────────────────────┐
│                         RtspPublisher                              │
│                                                                    │
│  rtsp://127.0.0.1:8554/live/mosaic                                 │
└────────────────────────────────────────────────────────────────────┘
```

## 2. main.cc 的职责

`main.cc` 是总装配层，负责把各个模块连接起来：

- 创建两个 RKNN 推理池：`testPool` 和 `facePool`。
- 创建两个 `YoloModelAdapter`：`yolo` 和 `face_yolo`。
- 把两个 adapter 加入 `MultiModelPipeline`。
- 创建 `FrameResultAggregator`，并设置必须等待的模型：

```cpp
aggregator.SetRequiredModels({"yolo","face_yolo"});
```

- 设置 aggregator 输出回调：

```text
ComposedFrame
  -> channels[ch]->OnFrameAggregated(frame_id)
  -> mosaic.Submit(frame)
```

- 创建 `MosaicComposer`，初始化 1920x1080、24fps 的四宫格输出。
- 创建 4 个 `VideoChannel`，每路读取一个 H264 测试流。
- 主循环不断从 `MultiModelPipeline` 拉取 `ModelOutput`，再交给对应通道进行重排和聚合。

## 3. VideoChannel 的职责

`VideoChannel` 表示一路视频输入。

每个通道内部主要有两个线程：

```text
DecodeLoop:
  读取 H264 文件
  调用 MppDecoder::DecodePacket()

OutputLoop:
  处理模型输出
  做 frame_id 重排
  将结果提交给 FrameResultAggregator
```

解码器每解出一帧后，会生成 `FrameContext`：

```text
channel_id
frame_id
pts_us
src_fd
src_buffer
width / height / stride
```

然后将这一帧提交给 `MultiModelPipeline`。

## 4. MultiModelPipeline 的职责

`MultiModelPipeline` 是多模型分发器。它不做推理，只负责把同一帧分发给多个模型分支。

当前逻辑是：

```text
FrameContext
  -> yolo adapter
  -> face_yolo adapter
```

每分发给一个模型分支时，会对 `src_buffer` 做一次 `mpp_buffer_inc_ref()`。这样多个模型分支可以异步使用同一帧，不会因为某个分支先结束而提前释放底层 buffer。

## 5. YoloModelAdapter 的职责

`YoloModelAdapter` 是统一模型接口和旧 YOLO 推理池之间的适配层。

它做两件事：

1. 提交时，将统一的 `FrameContext` 转成旧的 `input_data`，再调用 `rknnPool::put()`。
2. 取结果时，将旧的 `InferOutput` 转成统一的 `ModelOutput`。

因此，当前虽然两个分支都使用 `rkYolov5s`，但它们可以通过不同的 `model_id` 表示不同任务：

```text
model_id="yolo"
model_id="face_yolo"
```

后续如果再加一个 YOLO 权重，也可以继续使用不同的 `model_id`，例如：

```text
yolo-person
yolo-vehicle
yolo-fire
```

## 6. rknnPool 的职责

`rknnPool` 是 RKNN 推理线程池。

它内部维护多个 `rkYolov5s` 实例，并用线程池异步执行：

```text
put(input_data)
  -> 选择一个 model 实例
  -> 在线程池中执行 model->infer(inputData)
  -> 推理完成后放入 completed_outputs

get(output)
  -> 从 completed_outputs 取出推理结果
```

当前项目中有两个池：

```text
yoloPool: 3 threads
facePool: 9 threads
```

## 7. FrameResultAggregator 的职责

`FrameResultAggregator` 是多模型结果聚合层。

它使用：

```text
FrameKey = channel_id + frame_id
```

把同一帧的多个模型输出聚合到一起。

示例：

```text
输入：
  ModelOutput(ch0, frame100, yolo)
  ModelOutput(ch0, frame100, face_yolo)

聚合后：
  ComposedFrame(ch0, frame100, [yolo, face_yolo])
```

内部可以理解成：

```text
pending_[{ch0, frame100}]
  frame: ch0 frame100 的原始帧信息
  results:
    - yolo result
    - face_yolo result
```

如果 `required_models_` 中的模型都到齐，则输出完整 `ComposedFrame`。

如果等待超过 `timeout_`，则输出 `partial=true` 的 `ComposedFrame`，并记录缺失模型到 `missing_models`。

## 8. MosaicComposer 的职责

`MosaicComposer` 是四宫格合成层。

它保存四路最新帧：

```text
latest_[0]
latest_[1]
latest_[2]
latest_[3]
```

每个 `latest_` 中保存：

```text
原始图像 buffer
frame_id / pts_us
模型结果列表 model_results
```

`MosaicComposer` 不再由输入帧直接触发合成，而是由 `FlowLoop()` 按固定 fps 触发：

```text
每个 tick:
  如果四路都有 latest 帧
    -> RGA 缩放并贴到四宫格
    -> RenderModelResult 画模型结果
    -> PushBuffer 到 MPP 编码器
  否则
    -> 记录 not_ready
```

这样做的目的是隔离输入抖动，让编码器输入帧率更稳定。

## 9. RkMppEncoder 和 RtspPublisher 的职责

`RkMppEncoder` 负责硬件编码。

输入是四宫格合成后的 NV12 buffer：

```text
1920x1080 NV12
```

输出是 H264/H265 packet。

`RtspPublisher` 负责将编码后的 packet 推到 RTSP：

```text
rtsp://127.0.0.1:8554/live/mosaic
```

当前 mosaic 链路中的流控重点在编码器输入侧，也就是 `MosaicComposer::FlowLoop()`，而不是在编码器输出 callback 中 sleep。

## 10. 核心数据结构

### FrameContext

描述一帧原始图像的身份和内存信息：

```text
channel_id
frame_id
pts_us
src_fd
src_buffer
width / height / stride
```

### ModelOutput

描述某个模型对某一帧的输出：

```text
frame: FrameContext
result: ModelResult
```

### ModelResult

描述一个模型分支的结果：

```text
model_id
type
ok
error
inference_us
detections
```

### ComposedFrame

描述同一帧上聚合后的多个模型结果：

```text
frame: FrameContext
results: vector<ModelResult>
partial: 是否缺模型结果
missing_models: 缺失的模型 ID
```

## 11. 一句话总结

当前项目架构可以概括为：

```text
VideoChannel 负责解码和帧编号；
MultiModelPipeline 负责一帧多模型分发；
rknnPool 负责异步推理；
FrameResultAggregator 负责同帧多模型结果聚合；
MosaicComposer 负责四路画面固定帧率合成；
MppEncoder + RtspPublisher 负责编码推流。
```
