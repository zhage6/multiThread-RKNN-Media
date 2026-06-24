## Context

当前链路已经从输入触发式 mosaic 改为固定时钟输出：

```text
VideoChannel decode
  -> RKNN single-model pool
  -> per-channel reorder
  -> MosaicComposer latest cache
  -> fixed fps FlowLoop
  -> RGA mosaic
  -> MPP encoder
  -> RTSP publisher
```

这个结构适合实时显示：输入可以有抖动，输出按固定 fps 复用每路最新帧。多模型推理必须遵守这个实时模型，不能要求所有模型、所有通道、所有帧永远严格同步，否则会重新把输出节奏绑死在最慢模型上。

## Goals / Non-Goals

**Goals:**

- 支持一帧同时进入多个 RKNN 模型。
- 允许不同模型耗时不同、完成顺序不同。
- 使用 `channel_id + frame_id` 作为当前阶段的主同步键。
- 为未来真实 PTS 同步预留 `pts_us`。
- 避免模型结果画到错误帧上。
- 保持 mosaic fixed-clock 输出，不因某个模型慢而停止整个四宫格。
- 明确 buffer 生命周期：同一帧被多个模型/RGA 使用时，引用计数必须覆盖所有异步使用者。
- 保持现有 YOLO 模型可作为第一个模型适配器运行。

**Non-Goals:**

- 本阶段不实现跨摄像头真实 PTS 对齐。
- 本阶段不实现音频同步。
- 本阶段不实现复杂模型 DAG，例如模型 B 依赖模型 A 的裁剪结果。
- 本阶段不要求每个输出 tick 都包含所有模型的最新结果。
- 本阶段不重写 RKNN 模型内部推理逻辑，只抽象模型输入输出接口。

## Proposed Architecture

### 1. FrameContext

新增统一帧上下文，替代把 `input_data` 和 `InferOutput` 直接作为多模型核心协议：

```cpp
struct FrameContext {
    int channel_id;
    uint64_t frame_id;
    int64_t pts_us;

    int src_fd;
    MppBuffer src_buffer;

    int width;
    int height;
    int hor_stride;
    int ver_stride;
};
```

`FrameContext` 表示“同一张解码帧”。多模型 fan-out 时，每个模型任务都引用同一个 frame key。若模型会异步读取 `src_fd`，投递前必须增加 buffer 引用，模型完成后释放。

### 2. Model Identity and Result Envelope

每个模型需要稳定 ID：

```cpp
using ModelId = std::string; // "yolo", "classifier", "segmenter"
```

统一结果 envelope：

```cpp
enum class ModelResultType {
    Detection,
    Classification,
    Segmentation,
    Keypoints,
    Custom
};

struct ModelResult {
    ModelId model_id;
    ModelResultType type;
    bool ok;
    std::string error;
    uint64_t inference_us;

    // first phase: use typed optional payloads or variant-like structs
    detect_result_group_t detections;
};
```

第一阶段可以保留 `detect_result_group_t` 支持 YOLO，后续再把分类、分割等 payload 补齐。重点是下游不再假设“所有模型结果都是 YOLO 检测框”。

### 3. MultiModelPipeline

新增调度层负责 fan-out：

```text
FrameContext
   -> yolo pool task
   -> classifier pool task
   -> segmenter pool task
```

每个模型池可以是独立线程池/RKNN context 集合。模型输出统一回到 aggregator：

```text
ModelOutput {
  channel_id,
  frame_id,
  pts_us,
  model_id,
  result
}
```

### 4. FrameResultAggregator

聚合器以 `FrameKey(channel_id, frame_id)` 管理 pending frame：

```cpp
struct FrameAggregate {
    FrameContext frame;
    std::unordered_map<ModelId, ModelResult> results;
    std::chrono::steady_clock::time_point first_seen;
    uint32_t expected_mask;
    uint32_t done_mask;
};
```

策略：

- required 模型全部完成：发布该帧。
- optional 模型迟到：可被丢弃或只更新统计。
- required 模型超时：按配置丢帧或发布 partial。

第一阶段建议：

```text
required_models = ["yolo"]
optional_models = additional models
timeout = 80ms ~ 150ms
on timeout = publish partial
```

这样能先把框架跑通，不会因为新模型慢导致 mosaic 卡住。

### 5. Publishing to MosaicComposer

聚合完成后发布：

```cpp
struct ComposedFrame {
    FrameContext frame;
    std::vector<ModelResult> results;
    bool partial;
    std::vector<ModelId> missing_models;
};
```

`MosaicComposer::Submit()` 从 `InferOutput` 升级为 `ComposedFrame` 或新增 overload。它仍然只更新每通道 latest slot。`FlowLoop` 继续按 fixed fps 输出。

### 6. Rendering

渲染在 mosaic 输出 buffer 上发生，不应在单模型推理阶段改原始 decoder buffer。原因：

- 同一帧可能被多个模型读取，提前画框会污染其他模型输入。
- mosaic 输出缩放后需要将坐标映射到格子坐标。
- 不同模型结果需要不同渲染器。

设计：

```cpp
class ResultRenderer {
public:
    void Render(const MosaicInput& input,
                const im_rect& dst_rect,
                rga_buffer_t mosaic_dst);
};
```

第一阶段实现 detection renderer，将 YOLO box 从原图坐标缩放到 mosaic cell 坐标后调用 `imrectangle()`。

### 7. Frame Sync Semantics

需要区分两种同步：

1. 单帧多模型同步：
   - 同一 `channel_id + frame_id` 的多个模型结果必须聚合到同一帧。
   - 这是本变更必须实现的同步。

2. 四路画面时间同步：
   - fixed-clock mosaic 使用各路 latest frame。
   - 不强制四路同一 frame_id。
   - 后续有真实 PTS 后，可升级为按 tick 时间选择最接近的帧。

## Buffer Lifetime

当前单模型路径中，`mpp_buffer_inc_ref(data.src_buffer)` 保证推理完成后 frame buffer 仍可被 RGA/mosaic 使用。多模型 fan-out 后，必须改成显式 owner 计数：

```text
decode frame arrives
  -> create FrameContext with base src_buffer ref
  -> for each model task that reads src_fd, inc_ref
  -> model task done, put its ref
  -> aggregator/mosaic latest owns retained display ref
  -> latest replacement or Stop releases display ref
```

第一阶段可以简化：在 fan-out 前为每个模型任务 `mpp_buffer_inc_ref`，聚合器保留一个 display ref，所有模型完成/超时后释放任务 refs。

## Telemetry

新增日志：

- per-model submit fps / output fps / avg latency / timeout count。
- aggregator pending size / completed fps / partial publish fps / dropped late result count。
- per-channel latest frame id and model result coverage。
- mosaic render fps and per-model drawn result count。

## Risks / Trade-offs

- [Risk] 多模型 fan-out 增加 NPU/RGA 压力。  
  Mitigation: 每模型独立 inflight 上限，支持 optional 模型超时/跳帧。

- [Risk] required 模型过多会降低实时性。  
  Mitigation: 第一阶段只让核心模型 required，其他模型 optional。

- [Risk] 同一 decoder buffer 被多个模型/RGA 使用，生命周期更容易错。  
  Mitigation: 所有任务统一走 `FrameContext` 引用规则，禁止裸 fd 跨线程无 owner。

- [Risk] 结果渲染耦合模型类型。  
  Mitigation: 使用 renderer registry，按 `ModelResultType` 分发。

## Migration Plan

1. 新增 `FrameContext`、`ModelResult`、`ComposedFrame` 数据结构。
2. 为现有 YOLO 输出写适配器，将 `InferOutput` 转为 `ComposedFrame`。
3. 新增 `FrameResultAggregator`，先以单 required YOLO 模型运行，保持行为不变。
4. 将 `MosaicComposer::Submit()` 改为接收聚合帧，同时保留临时兼容 overload。
5. 增加 detection renderer，在 mosaic buffer 上画检测框。
6. 新增第二个模型适配器示例，验证 fan-out、聚合、超时和 partial publish。
7. 增加多模型健康日志。

## Open Questions

- 第一批多模型里，哪些是 required，哪些是 optional？
- 多模型是并行跑同一帧，还是部分模型只对检测框 crop 后再跑？
- 如果 optional 模型结果迟到，是丢弃还是允许更新下一次显示的 overlay？
- 每个模型的最大 inflight 应独立配置还是共享全局预算？
- 如果多模型总算力不足，优先保证哪个模型和哪个通道？
