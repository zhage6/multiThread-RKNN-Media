## Why

当前四宫格输出由输入帧到达事件触发合成。四路输入的解码、AI 推理和调度耗时存在抖动时，合成与编码输入节奏会随输入波动，容易表现为 VLC 端播放一顿一顿、局部马赛克糊化、短时间播放速度加快。

本变更希望先把四宫格输出改为统一时钟驱动：compositor 按固定周期产生编码输入帧，从而稳定最终推流帧率。当前阶段没有可靠 PTS，因此先使用每路 `frame_id` 作为帧标识，并预留 PTS 字段供后续真实时间戳对齐使用。

## What Changes

- 将四宫格合成从“输入帧触发”调整为“compositor 固定时钟触发”。
- 为每路输入保留独立帧缓存，第一阶段以 latest-frame 方式保存每个通道的最新可用帧。
- compositor 按目标 fps 周期取四路缓存中的帧，执行 scale/crop/pad 和四宫格合成。
- 编码器只接收 compositor 按统一节奏生成的合成帧，避免输入侧抖动直接传导到编码输入节奏。
- 当前阶段使用 `frame_id` 进行帧标识和日志观测，预留 `pts` 字段但不要求真实 PTS 对齐。
- 对缺帧、慢路、输出 buffer busy、RGA 失败等情况增加可观测统计，便于后续调试。
- 不在本阶段实现完整 jitter buffer、真实 PTS 排序、音视频同步或 RTSP packet 级缓冲。

## Capabilities

### New Capabilities

- `mosaic-frame-pacing`: 四路输入在 AI 后进入独立帧缓存，由 compositor 按统一时钟合成并稳定送入编码器。

### Modified Capabilities

- 无。

## Impact

- 主要影响 `MosaicComposer` 的帧接收、缓存、合成触发和资源释放逻辑。
- 可能影响 `InferOutput` / `MosaicInput` 的元数据字段，需要预留 `pts` 或等价时间戳字段。
- 影响 mosaic 编码输入节奏，但不改变解封装、解码、AI 推理主流程。
- 不引入新的第三方依赖；继续使用现有 MPP、RGA、RKNN 和 FFmpeg/RTSP 推流组件。
