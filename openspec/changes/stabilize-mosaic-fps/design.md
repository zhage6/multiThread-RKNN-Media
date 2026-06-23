## Context

当前四宫格链路中，四路输入已经完成了解封装、解码和 AI 推理，`MosaicComposer::Submit()` 收到的是 AI 后的原始帧及其 DMA/MPP buffer 信息。现有合成触发方式依赖输入帧到达：当四路 `latest_` 都有效时立即执行一次合成并送入编码器。

这种输入驱动模式会把四路解码、推理、线程调度和上游源抖动直接传递给编码输入。四路任意一路快慢变化，都会影响合成节奏。当前阶段还没有可用的真实 PTS，因此不能先实现完整的 jitter buffer 和按 PTS 对齐，但可以先建立稳定的输出时钟，把编码输入从输入抖动中隔离出来。

## Goals / Non-Goals

**Goals:**

- compositor 按配置 fps 生成固定周期 tick，例如 25fps 时每 40ms 触发一次合成。
- 每路输入帧进入独立缓存，第一阶段只保留最新帧，使用 `frame_id` 做帧标识和日志观测。
- 合成线程在每个 tick 上从四路缓存读取当前可用帧，RGA 合成为一张输出画面，再送入编码器。
- 合成完成后不立即释放 latest 输入帧，允许慢路在下一次 tick 复用上一帧。
- 预留 PTS 字段，当前允许填充无效值，后续接入真实 PTS 后升级为按时间戳选帧。
- 保持 MPP buffer 生命周期清晰：输入帧由通道缓存持有，被新帧覆盖、Stop、超时或通道失效时释放。
- 增加可观测日志，用于判断 submit fps、tick fps、compose fps、push fps、缺帧和丢帧情况。

**Non-Goals:**

- 本阶段不实现完整 jitter buffer 排序。
- 本阶段不做跨通道真实 PTS 对齐。
- 本阶段不做音频同步。
- 本阶段不新增 RTSP packet 级异步缓冲。
- 本阶段不改变解封装、解码和 AI 推理主流程。

## Decisions

### Decision 1: 控制编码器输入，而不是阻塞编码器输出

compositor 的固定时钟应控制送入编码器的原始合成帧。编码器输出 callback 中不应通过 sleep 来做流控，因为这会阻塞取 packet 和 buffer 回收路径，反而可能增加 MPP 内部拥塞风险。

替代方案是继续在编码输出 callback 中 sleep 推流。该方案看起来能控制 RTSP 写入速度，但它控制的是 packet 输出侧，不是原始帧产生侧；当编码输出线程被阻塞时，编码器内部 packet/buffer 回收可能变慢，不适合作为主流控点。

### Decision 2: 第一阶段使用 latest-frame 缓存

每个通道维护一个最新帧槽位。新帧到来时替换旧帧；合成 tick 读取各通道最新帧。该方案对没有真实 PTS 的阶段最稳妥，因为 `frame_id` 只能表示单通道帧序号，不能可靠表示跨通道同一时间点。

替代方案是直接实现每路 frame queue 并按 `frame_id` 对齐。该方案只有在四路输入同源、同步启动、同帧率且 frame_id 可比较时才可靠；普通四路输入下容易错误等待或错误丢帧。

### Decision 3: 预留 PTS，但当前不依赖 PTS

输入元数据应包含 `frame_id` 和预留 PTS 字段，例如 `pts_us` 或等价时间戳。当前没有真实 PTS 时，PTS 可以为 `-1` 或无效值，日志和调试仍以 `frame_id` 为主。后续接入真实 PTS 后，缓存结构可以从 latest-frame 升级为按 PTS 排序的小队列。

### Decision 4: 缺帧时复用上一帧，启动期未齐帧时等待

系统启动或某通道尚未收到首帧时，compositor 不应输出不完整四宫格，除非后续明确需要黑屏占位。四路至少各有一帧后，tick 可以持续合成；如果某路暂时没有新帧，则复用该路上一帧以保持输出 fps。

### Decision 5: 输出 buffer busy 时丢弃当前 tick，而不是清空输入缓存

当 mosaic 输出 buffer 暂时不可用时，本次 tick 应计入 busy/drop 并跳过合成或编码输入。它不应释放四路 latest 输入帧，因为这些帧仍可用于下一次 tick。清空 latest 会把短暂输出拥塞放大成后续缺帧。

## Risks / Trade-offs

- [Risk] latest-frame 模式不保证四路画面严格同一时刻。→ Mitigation: 当前目标是稳定推流 fps；后续有真实 PTS 后再升级为 frame queue + PTS 选帧。
- [Risk] 慢路会被复用旧帧，画面可能局部停顿。→ Mitigation: 增加每路重复帧/未更新统计，必要时后续加入超时黑屏或告警。
- [Risk] 固定 tick 线程和 Submit 线程竞争同一锁，RGA 合成期间可能短暂阻塞 Submit。→ Mitigation: 第一阶段保持实现简单；如后续发现锁竞争明显，再拆分缓存锁和合成资源锁。
- [Risk] 编码器实际 RC fps 与 compositor fps 不一致会影响码率控制。→ Mitigation: 后续将 fps 参数传入编码器配置，确保 MPP fps_in/fps_out 与 mosaic fps 一致。
- [Risk] 没有 RTSP packet 队列时，推流接口偶发阻塞仍可能反压编码输出。→ Mitigation: 本阶段先稳定编码输入；若日志显示 RTSP 写入抖动明显，再单独提出 packet queue change。

## Migration Plan

1. 保持解码和 AI 推理输出接口基本不变，先在输入元数据中预留 PTS 字段。
2. 将 `MosaicComposer::Submit()` 改为只更新每路缓存，不直接触发合成。
3. 在 `MosaicComposer` 中增加固定 fps 的 compositor 线程或等价定时循环。
4. 调整输入 buffer 释放时机：新帧覆盖旧帧、Stop 或超时清理时释放，合成成功后不立即释放 latest。
5. 移除编码输出 callback 中用于流控的 sleep，packet PTS 继续按输出帧序号生成。
6. 通过日志验证 submit/tick/compose/push fps 的关系。

## Open Questions

- 缺首帧时是否等待四路全部 ready，还是允许黑屏占位输出？
- 慢路复用上一帧超过多少毫秒后需要黑屏或标记异常？
- mosaic 输出 fps 是否固定为 25fps，还是继续由 `MosaicComposer::Init(..., fps)` 参数决定？
- 后续真实 PTS 的单位采用微秒、90kHz clock，还是沿用上游媒体时间基后统一转换？
