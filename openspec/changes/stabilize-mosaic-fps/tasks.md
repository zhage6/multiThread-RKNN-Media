## 1. Frame Metadata

- [x] 1.1 Add a reserved PTS field to the AI-to-mosaic frame metadata path, using an invalid value when true PTS is unavailable.
- [x] 1.2 Propagate `frame_id` and the reserved PTS field into `MosaicInput` so mosaic logs can observe both.

## 2. Per-Channel Frame Retention

- [x] 2.1 Keep one retained latest-frame slot for each of the four mosaic input channels.
- [x] 2.2 Update `Submit()` so it only validates the channel, releases the replaced frame for that channel, and stores the new retained frame.
- [x] 2.3 Ensure `Submit()` no longer calls the mosaic compose path directly when all channels are ready.
- [x] 2.4 Ensure retained input buffers are released on replacement, `Stop()`, and initialization failure cleanup.

## 3. Fixed-Clock Compositor

- [x] 3.1 Add a compositor pacing loop driven by the configured mosaic fps.
- [x] 3.2 Start the pacing loop after encoder and publisher initialization succeed.
- [x] 3.3 Stop and join the pacing loop before releasing retained input frames, encoder, publisher, or output buffers.
- [x] 3.4 On each tick, compose only after all four channels have at least one retained frame.
- [x] 3.5 Reuse retained frames across ticks instead of releasing them after each successful compose.

## 4. Encoder and Output Behavior

- [x] 4.1 Remove sleep-based pacing from the encoder output callback.
- [x] 4.2 Continue generating monotonic encoded packet PTS/DTS from the encoded mosaic frame index and configured fps.
- [x] 4.3 When mosaic output buffer acquisition fails, count the busy/drop event and keep retained input frames for later ticks.
- [ ] 4.4 Verify encoder input push count follows the compositor tick cadence when encoder resources are available.

## 5. Telemetry and Validation

- [x] 5.1 Extend mosaic health logs with tick fps, compose fps, push fps, busy/drop fps, and latest frame ids for all channels.
- [x] 5.2 Add missing-frame or not-ready tick statistics for startup and slow-channel observation.
- [ ] 5.3 Validate on board that four initialized channels produce stable mosaic push fps near the configured fps.
- [ ] 5.4 Validate that one bursty or faster input channel does not increase mosaic encode input fps above the configured fps.
- [ ] 5.5 Validate that temporary output buffer pressure does not clear retained input frames.
