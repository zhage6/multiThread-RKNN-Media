## Why

当前系统已经实现四路输入、RKNN 推理、固定时钟四宫格合成和统一 RTSP 推流。现有推理链路仍以单一 YOLO 模型为中心：`rknnPool<rkYolov5s, input_data, InferOutput>` 输出一个检测结果，`MosaicComposer` 保存每路最新推理帧并按固定 fps 合成。

下一步需要支持多模型推理，例如检测、分类、分割、关键点或其他 RKNN 模型组合，并且不希望系统只绑定 YOLO 的 `detect_result_group_t`。多模型会引入新的同步问题：同一帧可能被送入多个模型，模型完成顺序不同，耗时不同，某个模型也可能丢结果或超时。系统必须能把多个模型结果重新聚合到同一帧，再决定这帧是否进入 mosaic latest 缓存，或是否允许部分结果先显示。

本变更希望建立一个模型无关的推理结果聚合层，让单帧在多模型异步完成后仍能按 `channel_id + frame_id` 对齐，避免不同模型的结果画到错误帧上，同时保持实时四宫格的固定输出节奏。

## What Changes

- 引入模型无关的帧上下文 `FrameContext`，用 `channel_id`、`frame_id`、`pts_us`、源 DMA fd/MppBuffer 和图像尺寸描述一帧。
- 引入模型任务描述 `ModelTask` 和模型输出容器 `ModelResult`，不再让下游直接依赖 YOLO 的具体输出结构。
- 引入多模型调度层 `MultiModelPipeline`，负责把一帧 fan-out 到多个模型池，收集异步结果，并按帧聚合。
- 引入帧结果聚合器 `FrameResultAggregator`，以 `channel_id + frame_id` 为键追踪同一帧的多个模型结果。
- 聚合器支持两种策略：
  - required 模型全部完成后才发布该帧。
  - 超时后发布部分结果，并记录 missing model。
- `MosaicComposer::Submit()` 接收聚合后的 `ComposedFrame` 或等价结构，继续采用 fixed-clock latest 模式输出。
- 结果渲染从 YOLO 专属画框扩展为模型结果渲染接口，例如 detection boxes、segmentation mask、classification label 等。
- 保留现有单模型 YOLO 路径作为第一阶段兼容适配器，避免一次性重写 RKNN 推理实现。

## Capabilities

### New Capabilities

- `multi-model-inference`: 同一输入帧可以被投递到多个 RKNN 模型，模型输出以统一结果格式回到聚合层。
- `frame-result-synchronization`: 多模型异步结果按 `channel_id + frame_id` 聚合，避免跨帧错配。
- `partial-result-timeout`: 当某些模型结果迟到或失败时，系统可以按配置超时发布部分结果，保持实时性。
- `model-result-rendering`: mosaic 输出支持按模型类型渲染不同结果，而不是只支持 YOLO 检测框。

### Modified Capabilities

- `mosaic-frame-pacing`: mosaic 继续按固定 fps 输出，但输入从单模型 `InferOutput` 升级为聚合后的 frame result。

## Impact

- 主要影响 `rknnPool` 使用方式、`rkYolov5s` 输出适配、`StreamChannel` 的推理投递路径、`MosaicComposer` 的输入结构和渲染逻辑。
- 需要新增多模型相关头文件/实现，例如 `FrameContext`、`ModelResult`、`MultiModelPipeline`、`FrameResultAggregator`。
- 需要定义每个模型的生命周期和 buffer 引用规则，确保同一解码帧 fan-out 到多个模型时不会过早释放 `MppBuffer`。
- 需要扩展日志以观察每个模型的 fps、延迟、超时、丢结果、聚合队列长度和 mosaic 使用的结果版本。
- 不改变当前 MPP 解码、DMA buffer 分配、fixed-clock mosaic 输出和 RTSP 推流的基本架构。
