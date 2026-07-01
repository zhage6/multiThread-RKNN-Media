# 帧同步策略与 Mosaic 输出策略

这份文档说明当前项目里两件核心事情：

```text
1. 多模型推理结果如何回到同一帧
2. 四宫格 mosaic 为什么按 latest 最新帧策略输出
```

当前项目不是简单的“一帧进，一帧出”。因为一帧会被分发给多个模型，多个模型又在多个线程里异步返回，所以系统必须做帧同步。

## 总体策略

```text
解码帧
  -> 分发给多个模型
  -> 每个模型异步返回 ModelOutput
  -> 每路 VideoChannel 先按 frame_id 重排
  -> FrameResultAggregator 按 channel_id + frame_id 聚合多模型结果
  -> 聚合完成后输出 ComposedFrame
  -> MosaicComposer 保存每路最新 ComposedFrame
  -> flow_thread 按固定 fps 合成四宫格并推流
```

可以理解成三层同步：

```text
第一层：每一路内部的帧顺序同步
  解决 RKNN 多线程乱序返回问题

第二层：同一帧的多模型结果同步
  解决 yolo、face_yolo 等模型完成时间不同的问题

第三层：四路 mosaic 输出节奏同步
  解决多路视频更新速度不同的问题
```

## 1. 每路帧顺序同步

位置：`VideoChannel::ProcessModelOutput()`

核心变量：

```text
m_expected_frame_id
m_model_reorder_buffer
m_reorder_waiting
m_reorder_wait_start
m_reorder_timeout
m_reorder_skipped_frames
```

模型推理是多线程的，所以同一路帧可能乱序返回：

```text
输入顺序:
  frame 100
  frame 101
  frame 102

返回顺序:
  frame 100
  frame 102
  frame 101
```

如果不重排，后面聚合器和 mosaic 会看到乱序视频帧，检测框也可能对应错画面。

当前策略是：

```text
收到 ModelOutput
  │
  ├─ 如果 frame_id < m_expected_frame_id
  │     ├─ 如果该帧已被 skip，直接释放
  │     └─ 否则仍提交给 aggregator
  │
  └─ 否则放入 m_model_reorder_buffer[frame_id]
        │
        ▼
      检查 m_expected_frame_id 是否已经回来
        │
        ├─ 回来了：
        │     -> 将这一帧的所有 ModelOutput 提交给 aggregator
        │     -> m_expected_frame_id++
        │     -> 继续检查下一帧
        │
        └─ 没回来：
              -> 如果等待未超时，继续等
              -> 如果等待超时，跳过缺失帧
```

重排的目标不是保证每一帧都必须回来，而是保证：

```text
能按顺序出的帧按顺序出
缺失太久的帧不要卡死后续视频
```

### 重排超时

如果当前期待的帧迟迟没回来：

```text
now - m_reorder_wait_start >= m_reorder_timeout
```

系统会：

```text
1. 记录 model_reorder_timeout
2. 把缺失 frame_id 记入 m_reorder_skipped_frames
3. 调用 aggregator->SkipFrame(channel_id, frame_id)
4. 调用 OnFrameAggregated(frame_id)，释放该路 inflight 名额
5. m_expected_frame_id++
```

这个策略的意义是：不能让一个丢失或极慢的模型结果卡住整路视频。

## 2. 多模型结果同步

位置：`FrameResultAggregator`

核心变量：

```text
queue_
pending_
required_models_
expected_publish_frame_
timeout_
finished_frames_
finished_order_
```

每个模型输出的是：

```cpp
ModelOutput {
    FrameContext frame;
    ModelResult result;
}
```

聚合器按这个 key 合并：

```text
FrameKey = channel_id + frame_id
```

例如：

```text
ch=0 frame=120
  yolo result 到了
  face_yolo result 还没到

pending_[{0,120}]
  frame = ch0 frame120
  results = [yolo]
```

等 face_yolo 到了：

```text
pending_[{0,120}]
  frame = ch0 frame120
  results = [yolo, face_yolo]
```

此时如果满足 required models，就可以发布完整 `ComposedFrame`。

## 3. required_models 的意义

当前 main 里配置：

```cpp
aggregator.SetRequiredModels({"yolo","face_yolo"});
```

这表示一帧要正常完整发布，需要同时具备：

```text
yolo result
face_yolo result
```

聚合器判断逻辑是：

```text
对 required_models_ 里的每个 model_id：
    pending results 里必须找到同名且 ok 的 result

全部找到：
    ready = true
否则：
    ready = false
```

这个策略适合你现在的双模型 demo，因为你希望同一帧同时带两个模型的结果。

但它也意味着：最慢的 required model 会影响这一帧发布时间。

## 4. 聚合超时与 partial frame

位置：`FrameResultAggregator::PublishAvailableLocked()`

聚合器不会无限等 required models。

它会判断：

```text
ready = required models 是否都到了
timed_out = 等待时间是否超过 timeout_
```

发布条件是：

```text
ready == true
或者
timed_out == true
```

如果超时但没有等齐，则输出：

```cpp
ComposedFrame {
    frame;
    results;
    partial = true;
    missing_models = ...
}
```

这个策略是在完整性和实时性之间折中：

```text
等齐：
  结果完整，但慢模型会增加延迟

超时 partial：
  结果可能缺模型，但视频不会一直卡死
```

对于实时视频，这个超时机制是必须的。否则只要某个模型某一帧异常，整路就会卡住。

## 5. 聚合器的顺序门控

核心变量：

```text
expected_publish_frame_[channel_id]
```

聚合器不只是合并多模型结果，它还会保证每一路按 frame_id 顺序发布。

逻辑是：

```text
对每个 channel_id：
    只检查 expected_publish_frame_[channel_id]

如果这一帧存在于 pending_：
    ready 或 timeout 后发布
    expected_publish_frame_[channel_id]++
    继续检查下一帧

如果这一帧不存在：
    停止，不跨过它
```

所以即使后面的帧已经聚合好了，也不会直接越过前面的帧发布，除非前面的帧被 `SkipFrame()` 标记跳过。

这个门控保证的是：

```text
每一路输出到 mosaic 的 ComposedFrame 是有序的
```

## 6. SkipFrame 的意义

位置：`FrameResultAggregator::SkipFrame()`

当 `VideoChannel` 的重排层发现某个 frame_id 超时，它会通知聚合器：

```cpp
m_aggregator->SkipFrame(m_channel_id, skipped_frame_id);
```

聚合器会：

```text
1. 如果 pending_ 里已经有这帧的部分结果，释放并删除
2. 把这帧记入 finished_frames_
3. 推进 expected_publish_frame_
4. 尝试继续发布后面的帧
```

这一步很关键。

否则会出现这种问题：

```text
VideoChannel 已经决定跳过 frame 100
但 aggregator 还在等待 frame 100
结果 frame 101、102、103 都被卡在后面
```

`SkipFrame()` 的作用就是让重排层和聚合层对“跳过某帧”达成一致。

## 7. finished_frames_ 的意义

核心变量：

```text
finished_frames_
finished_order_
max_finished_history_
```

它记录已经发布或跳过的帧。

用途是处理迟到结果：

```text
frame 100 已经超时跳过
过了一会儿，某个模型的 frame 100 结果才回来
```

这时候不能再让它进入 pending_，否则会污染当前顺序，甚至造成 buffer 生命周期混乱。

所以聚合器在 `Submit()` 和 `AddResult()` 中会检查：

```text
如果这个 FrameKey 已经 finished：
    释放 output
    不再参与聚合
```

`finished_order_` 用来限制历史记录长度，避免 `finished_frames_` 无限增长。

## 8. Mosaic 的 latest 策略

位置：`MosaicComposer`

核心变量：

```text
latest_[4]
flow_thread_
fps_
mosaic_grp_
```

`MosaicComposer::Submit()` 不会把每一帧排成 FIFO 队列，而是按通道覆盖：

```text
latest_[channel_id] = 最新 ComposedFrame
```

如果这一通道原来有旧帧：

```text
ReleaseInput(slot)
```

然后保存新帧。

也就是说，mosaic 的输入策略是：

```text
每路只保留最新一帧
旧帧会被覆盖释放
```

它不是完整帧队列，不保证每个输入帧都合成输出。

## 9. 为什么 Mosaic 不严格等四路同 frame_id

四路输入本来就不是严格同步的。

即使都是本地文件，不同路也会因为：

```text
解码速度不同
模型推理耗时不同
重排等待不同
聚合超时不同
Linux 调度不同
```

导致每路最新 frame_id 不一致。

如果 mosaic 强制要求：

```text
ch0 frame 100
ch1 frame 100
ch2 frame 100
ch3 frame 100
```

都到齐才输出，那么最慢的一路会拖住整个四宫格。

结果会变成：

```text
某一路慢
  -> 全部 mosaic 停住
  -> RTSP 输出不稳定
  -> 延迟越来越大
```

所以当前采用实时视频更常见的策略：

```text
每路各自保证内部顺序
mosaic 不强求四路 frame_id 完全一致
mosaic 按固定 fps 取每路 latest
```

这会牺牲“严格四路同一时刻”，但换来：

```text
低延迟
输出节奏稳定
慢通道不会拖死全部输出
```

## 10. Mosaic 输出节奏

位置：`MosaicComposer::FlowLoop()`

`flow_thread_` 按固定周期运行：

```text
period = 1000000 / fps
next_tick += period
```

每次 tick：

```text
如果 latest_[0..3] 都 valid：
    ComposeLocked()
否则：
    mosaic_not_ready_count_++
```

所以 mosaic 是固定 fps 主动拉取 latest，而不是被输入帧直接驱动。

这点很重要：

```text
输入帧来得快：
    latest 会覆盖旧帧，mosaic 仍按固定 fps 输出

输入帧来得慢：
    mosaic 可能重复使用旧 latest，或者 not ready
```

## 11. Mosaic 合成策略

位置：`MosaicComposer::ComposeLocked()`

每次合成：

```text
1. 从 mosaic_grp_ 取一个输出 MppBuffer
2. 计算四宫格 cell 尺寸
3. 遍历 latest_[0..3]
4. 每一路用 RGA improcess 缩放贴到对应区域
5. 根据 model_results 画检测框/关键点
6. 调用 encoder_->PushBuffer(dst_buffer)
```

四宫格布局：

```text
┌──────────────┬──────────────┐
│    ch0       │    ch1       │
├──────────────┼──────────────┤
│    ch2       │    ch3       │
└──────────────┴──────────────┘
```

对应代码逻辑：

```text
cell_w = out_width_ / 2
cell_h = out_height_ / 2

dst_rect.x = (i % 2) * cell_w
dst_rect.y = (i / 2) * cell_h
```

## 12. 当前策略的取舍

当前系统选择的是实时优先策略：

```text
每路内部：
  尽量保证 frame_id 顺序
  但超时会跳帧

多模型：
  尽量等 required models
  但超时会 partial 输出

四宫格：
  不要求四路同 frame_id
  只取每路 latest

推流：
  按固定 fps 输出 mosaic
```

这套策略适合实时推流，因为它避免了几个危险：

```text
单帧缺失导致整路卡死
慢模型导致所有帧无限堆积
慢通道导致整个四宫格停住
编码前堆积导致延迟持续增大
```

代价是：

```text
可能跳过个别帧
可能输出 partial 结果
四路画面 frame_id 不一定完全一致
某一路慢时，mosaic 可能重复使用旧画面
```

## 13. 后续可调参数

当前最值得根据实际效果调的参数：

```text
m_reorder_timeout
  每路等待缺失 frame_id 的时间

FrameResultAggregator::timeout_
  等多模型结果到齐的时间

required_models_
  哪些模型必须等，哪些模型允许缺失

m_max_inflight_frames
  每路最多允许多少帧在后级链路中飞行

pipeline.PendingCount() 阈值
  全局模型任务堆积到多少开始反压解码

Mosaic fps
  四宫格输出帧率
```

调参方向：

```text
想更低延迟：
  降低 timeout
  减少 inflight
  慢模型不要 required

想更多完整结果：
  提高 timeout
  允许更大 inflight
  required_models 放更多模型

想输出更稳：
  保持 latest 覆盖策略
  不要改成四路严格同 frame_id 等待
```

## 简短总结

当前帧同步不是为了“每一帧绝对完整”，而是为了在实时系统里做到：

```text
每路尽量有序
多模型尽量对齐
超时可以恢复
四宫格稳定输出
整体延迟不无限增长
```

这也是当前多模型四路 mosaic 推流最重要的设计取舍。
