## 1. Data Model

- [x] 1.1 Add `FrameContext` with channel id, frame id, reserved PTS, source fd, source MPP buffer, dimensions, and strides.
- [x] 1.2 Add `ModelId`, `ModelResultType`, and `ModelResult` envelope.
- [x] 1.3 Add `ComposedFrame` as the object published from aggregation into mosaic.
- [x] 1.4 Add compatibility conversion from current YOLO `InferOutput` into `ComposedFrame`.

## 2. Buffer Ownership

- [ ] 2.1 Define ownership rules for fan-out model tasks using `mpp_buffer_inc_ref` / `mpp_buffer_put`.
- [ ] 2.2 Ensure each model task releases its source buffer reference after inference.
- [x] 2.3 Ensure the aggregator or mosaic latest slot owns exactly one display reference.
- [x] 2.4 Add failure cleanup for model submit failure, timeout, and late result discard.

## 3. Model Adapter Interface

- [x] 3.1 Introduce a model adapter interface with `Submit(FrameContext)` and model result output.
- [x] 3.2 Wrap existing `rkYolov5s` / `rknnPool` path as the first adapter.
- [ ] 3.3 Add model config describing model id, required/optional status, max inflight, timeout, and render type.
- [x] 3.4 Keep the single YOLO behavior working through the new adapter path.

## 4. Multi-Model Scheduling

- [x] 4.1 Add `MultiModelPipeline` to fan-out a `FrameContext` to configured model adapters.
- [ ] 4.2 Add per-model inflight limits so slow optional models do not consume all frame buffers.
- [ ] 4.3 Add handling for model submit failure and inference failure.
- [ ] 4.4 Add health logs for per-model submit fps, output fps, latency, and failure counts.

## 5. Frame Result Aggregation

- [x] 5.1 Add `FrameResultAggregator` keyed by `channel_id + frame_id`.
- [x] 5.2 Track expected and completed model results per frame.
- [x] 5.3 Publish a frame when all required models are complete.
- [x] 5.4 Add timeout behavior for missing required or optional models.
- [x] 5.5 Drop late model results for frames that have already been published or discarded.
- [ ] 5.6 Add logs for pending size, completed fps, partial fps, timeout count, and late drop count.

## 6. Mosaic Integration

- [x] 6.1 Change `MosaicComposer::Submit()` to accept `ComposedFrame` or add a new overload.
- [x] 6.2 Store per-channel latest frame plus all aggregated model results.
- [x] 6.3 Keep fixed-clock `FlowLoop` behavior unchanged.
- [x] 6.4 Ensure replacing latest frame releases the old display buffer reference.
- [x] 6.5 Ensure `Stop()` releases retained multi-model frame data safely.

## 7. Rendering

- [x] 7.1 Move YOLO detection box drawing into `MosaicComposer` render path on the mosaic output buffer.
- [x] 7.2 Scale detection coordinates from source frame coordinates to mosaic cell coordinates.
- [x] 7.3 Add renderer dispatch by `ModelResultType`.
- [x] 7.4 Add no-op renderer for unsupported result types to avoid blocking integration.
- [ ] 7.5 Add render logs with drawn box count and unsupported result count.

## 8. Validation

- [ ] 8.1 Validate single-model YOLO path produces the same mosaic output as before, now through the aggregation path.
- [ ] 8.2 Validate two-model mode where one model is slower and optional; mosaic should continue fixed fps with partial results.
- [ ] 8.3 Validate required model timeout behavior does not leak buffers.
- [ ] 8.4 Validate frame ids in rendered results match the source frame used for each mosaic cell.
- [ ] 8.5 Validate long-run stability: no increasing pending counts, no MPP buffer exhaustion, and stable mosaic push fps.
