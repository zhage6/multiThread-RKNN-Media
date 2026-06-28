# ComposedFrame 内存管理学习笔记

## 1. 这套结构解决什么问题

当前项目从单 YOLO 推理，开始向多模型推理演进。原来的 `InferOutput` 更像是：

```text
一帧图像 + 一个 YOLO 结果
```

而新的 `ComposedFrame` 目标是：

```text
一帧图像 + 多个模型结果
```

所以现在的数据结构分成了几层：

```text
FrameContext
  描述一帧图像本身：channel_id、frame_id、src_fd、src_buffer、宽高、stride

ModelResult
  描述某一个模型的输出：model_id、type、ok、detections 等

ModelOutput
  某个模型对某一帧的输出：FrameContext + ModelResult

ComposedFrame
  聚合完成后的一帧：FrameContext + vector<ModelResult>
```

这里最关键的是：`ComposedFrame` 里的 `results` 是 `vector`，因为同一帧后面可能同时有 YOLO、分类、分割、关键点等多个模型结果。

## 2. src_fd 和 src_buffer 的关系

在当前管线里，一帧解码图像底层是 MPP buffer / dma-buf。

```text
src_fd
  用户态传给 RGA / MPP / NPU 的 dma-buf fd

src_buffer
  MPP 对这块 buffer 的管理句柄，负责引用计数和生命周期
```

`src_fd` 更像是硬件之间共享这块内存的钥匙。  
`src_buffer` 是这块内存的 MPP 生命周期句柄。

所以真正决定这块解码 buffer 什么时候能还给 MPP 的，是 `src_buffer` 的引用计数。

## 3. 基本所有权规则

当前设计里，一帧 buffer 的 display 引用只能被一个阶段持有。

```text
VideoChannel / RKNN 输出
  -> FrameResultAggregator
  -> MosaicComposer latest slot
  -> 被新帧替换或 Stop 时释放
```

核心规则：

```text
谁继续持有 src_buffer，谁负责最终 mpp_buffer_put。
谁把 src_buffer 交给下游，谁就不能再释放它。
如果没有成功交给下游，必须自己释放。
```

## 4. 正常路径

单模型 YOLO 当前路径可以理解为：

```text
MPP 解码出 frame
  -> VideoChannel inc_ref(src_buffer)
  -> RKNN 推理
  -> InferOutput 回来
  -> MakeYoloComposedFrame()
  -> ModelOutput 提交给 FrameResultAggregator
  -> Aggregator 发布 ComposedFrame
  -> MosaicComposer::Submit(ComposedFrame)
  -> MosaicComposer latest_[channel] 持有 src_buffer
```

当 `MosaicComposer` 收到同一路的新帧时：

```text
ReleaseInput(old latest slot)
  -> mpp_buffer_put(old src_buffer)

latest slot = new frame
```

所以正常情况下，aggregator 发布成功后不释放这帧，因为所有权已经转移给 `MosaicComposer`。

## 5. MakeYoloComposedFrame 的意义

`MakeYoloComposedFrame()` 是兼容旧 YOLO 路径的适配器。

它把：

```text
InferOutput
```

转换成：

```text
ComposedFrame {
    frame = 当前帧信息
    results = [ YOLO 检测结果 ]
}
```

当前虽然只有一个 YOLO 结果，但仍然使用：

```cpp
composed.results.push_back(result);
```

原因是后面多模型时，同一帧会变成：

```text
ComposedFrame {
    frame = 第 0 路第 123 帧
    results = [
        yolo_result,
        classification_result,
        segmentation_result
    ]
}
```

`vector<ModelResult>` 不是用来寻找同一帧的。  
同一帧是靠 `channel_id + frame_id` 匹配的。  
`vector<ModelResult>` 只是用来装这一帧已经聚合到的多个模型结果。

## 6. FrameResultAggregator 的所有权

`FrameResultAggregator` 用：

```text
FrameKey = channel_id + frame_id
```

把多个模型结果聚合到同一个 `FrameAggregate`。

```text
pending_[{channel_id, frame_id}]
  -> FrameAggregate {
       frame: 这一帧的图像引用
       results: 多个模型结果
     }
```

这里必须保证：

```text
一个 FrameAggregate 只持有一份 src_buffer 引用
```

也就是说，同一帧如果有多个模型结果回来：

```text
第一个模型结果
  -> agg.frame 接管 output.frame.src_buffer

第二个模型结果
  -> 只留下 output.result
  -> 释放 output.frame.src_buffer

第三个模型结果
  -> 只留下 output.result
  -> 释放 output.frame.src_buffer
```

否则每个模型结果都带一份 `src_buffer` 引用进来，就会造成引用泄漏。

推荐 `AddResult()` 的核心逻辑是：

```cpp
bool first_result = agg.results.empty();

if (first_result) {
    agg.frame = output.frame;
    agg.first_seen = std::chrono::steady_clock::now();
}

agg.results.push_back(std::move(output.result));

if (!first_result) {
    ReleaseFrame(output.frame);
}
```

## 7. 发布后的所有权转移

当 required models 都齐了，或者等待超时，aggregator 会生成：

```cpp
ComposedFrame frame = MakeComposedFrame(agg, partial);
```

然后回调：

```cpp
cb(frame);
```

如果这个 `cb` 是 `mosaic.Submit(frame)`，那么所有权转移给 `MosaicComposer`。

```text
Aggregator
  发布 ComposedFrame
  不再释放 frame.src_buffer

MosaicComposer
  latest slot 保存 frame.src_buffer
  后续替换或 Stop 时释放
```

如果没有 callback，则没有下游接管，aggregator 必须释放：

```cpp
if (cb) {
    cb(frame);
} else {
    ReleaseFrame(frame.frame);
}
```

## 8. Stop 时为什么不能直接 clear

`queue_` 和 `pending_` 里可能还保存着没发布出去的帧。

错误做法：

```cpp
queue_.clear();
pending_.clear();
```

这样会直接丢掉 C++ 对象，但不会自动调用 `mpp_buffer_put()`，MPP buffer 引用会泄漏。

正确做法：

```cpp
for (auto& output : queue_) {
    ReleaseOutput(output);
}
queue_.clear();

for (auto& item : pending_) {
    ReleaseAggregate(item.second);
}
pending_.clear();
```

含义是：

```text
未发布出去的帧，还属于 aggregator
Stop 清理时必须由 aggregator 释放
```

## 9. 超时和迟到结果

多模型时，某些模型可能比较慢。

例如：

```text
frame 123:
  yolo 20ms 回来
  seg  200ms 回来

timeout = 120ms
```

120ms 时 aggregator 可能已经发布 partial frame：

```text
frame 123 partial=true
missing_models = ["seg"]
```

这时 `frame 123` 的聚合窗口已经关闭。

如果 200ms 时 `seg` 才回来，这个结果就是 late result。

late result 的正确处理：

```text
发现 frame key 已经 finished
  -> ReleaseOutput(output)
  -> 不进入 pending_
  -> 不重新发布旧帧
```

否则会出现：

```text
旧帧重新创建 pending
旧帧再次发布到 Mosaic
画面时间线倒退
buffer 被额外占住
```

所以需要记录最近完成的帧：

```cpp
std::set<FrameKey> finished_frames_;
std::deque<FrameKey> finished_order_;
```

发布成功或超时发布后：

```cpp
RememberFinishedLocked(key);
```

新的结果进入时：

```cpp
if (IsFinishedLocked(key)) {
    ReleaseOutput(output);
    return true;
}
```

## 10. MosaicComposer 的所有权

`MosaicComposer::Submit(const ComposedFrame& frame)` 接收一帧后，会把它保存到对应通道的 latest slot：

```text
latest_[channel_id] = frame
```

如果这个通道原来已有旧帧：

```cpp
ReleaseInput(old_slot);
```

然后新帧成为 latest。

所以 `MosaicComposer` 的规则是：

```text
每个通道 latest slot 最多持有一帧 src_buffer
新帧替换旧帧时释放旧 buffer
Stop / 析构时释放所有 latest buffer
```

固定帧率拼接时，如果某一路暂时没有新帧，`MosaicComposer` 会复用 latest slot 的旧帧。这意味着 latest slot 必须一直持有对应的 `src_buffer`，直到被更新。

## 11. 常见错误

### 11.1 发布成功后又释放

错误：

```text
Aggregator cb(frame) 给 Mosaic 后，又 mpp_buffer_put(frame.src_buffer)
```

后果：

```text
Mosaic 还在用这块 fd，但底层 buffer 可能已经还给 MPP
可能出现花屏、RGA 失败、随机错误
```

### 11.2 未发布就 clear

错误：

```text
queue_ / pending_ 直接 clear
```

后果：

```text
src_buffer 引用泄漏
解码外部 buffer 池逐渐耗尽
```

### 11.3 多模型结果都持有 frame

错误：

```text
yolo output 持有 frame buffer
cls output 也持有同一帧 buffer
seg output 也持有同一帧 buffer
全部塞进同一个 aggregate 后不释放多余引用
```

后果：

```text
每帧引用计数多加几次
buffer 释放明显变晚
多路多模型时很容易耗尽
```

### 11.4 late result 重新进入 pending

错误：

```text
frame 123 已经超时发布
seg_result frame 123 晚到
重新创建 pending_[123]
```

后果：

```text
旧帧复活
画面可能倒退或闪烁
buffer 多占
```

## 12. 当前检查清单

实现多模型前，至少确认这些点：

```text
[ ] VideoChannel 投递模型任务前，按模型数量正确 inc_ref
[ ] 每个模型任务推理完成后，释放自己的任务引用
[ ] Aggregator 每个 FrameAggregate 只持有一份 display 引用
[ ] Aggregator Stop 时释放 queue_ 和 pending_ 中未发布帧
[ ] Aggregator callback 为空时释放 frame
[ ] Aggregator 对 late result 直接 ReleaseOutput
[ ] MosaicComposer 替换 latest slot 时释放旧帧
[ ] MosaicComposer Stop 时释放所有 latest slot
```

## 13. 一句话总结

`ComposedFrame` 不是单纯的数据包，它携带着一帧 dma-buf 的生命周期。  

所以这条线必须一直清楚：

```text
这份 src_buffer 现在归谁？
下游有没有成功接管？
如果没有接管，当前阶段有没有 put？
```

只要这三个问题始终回答得上来，MPP 解码 buffer、RGA 输入、Mosaic latest slot、多模型聚合之间的内存管理就不会乱。
