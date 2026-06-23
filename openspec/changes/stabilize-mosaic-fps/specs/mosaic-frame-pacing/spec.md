## ADDED Requirements

### Requirement: Compositor uses a fixed output clock
The mosaic compositor SHALL generate mosaic frames according to the configured output fps instead of directly composing on every input frame arrival.

#### Scenario: 25fps output pacing
- **WHEN** the mosaic compositor is initialized with 25fps and all four channels have at least one valid frame
- **THEN** it produces at most one encoded-input mosaic frame for each 40ms output tick

#### Scenario: Input burst isolation
- **WHEN** one or more input channels submit multiple frames between two output ticks
- **THEN** the compositor uses the latest retained frame for that channel at the next tick and does not produce extra mosaic frames for the burst

### Requirement: Each channel retains an independent latest frame
The system SHALL maintain an independent retained frame slot for each of the four input channels.

#### Scenario: New frame replaces old retained frame
- **WHEN** a channel submits a new valid frame while that channel already has a retained frame
- **THEN** the old retained frame is released and the new frame becomes the retained frame for that channel

#### Scenario: Compose does not consume retained frames
- **WHEN** the compositor successfully uses retained frames to compose a mosaic frame
- **THEN** the retained input frames remain available for later output ticks unless replaced, stopped, or explicitly expired

### Requirement: Frame metadata reserves PTS
The system SHALL carry frame identity metadata containing the existing `frame_id` and a reserved PTS field for future timestamp-based alignment.

#### Scenario: Current phase uses frame_id
- **WHEN** true media PTS is not available from the input path
- **THEN** the compositor logs and tracks frames by `frame_id` while keeping the reserved PTS field invalid or unset

#### Scenario: Future PTS compatibility
- **WHEN** true media PTS becomes available later
- **THEN** the existing frame metadata shape can carry that PTS without changing the compositor input contract again

### Requirement: Missing frames do not destabilize output cadence
The compositor SHALL preserve output cadence when a channel temporarily has no newer frame after startup.

#### Scenario: Startup waits for first frames
- **WHEN** fewer than four channels have submitted an initial valid frame
- **THEN** the compositor does not send incomplete mosaic frames to the encoder

#### Scenario: Slow channel reuses retained frame
- **WHEN** all four channels have initialized and one channel has no newer frame for a later output tick
- **THEN** the compositor may reuse that channel's retained frame while continuing to generate mosaic frames at the configured output cadence

### Requirement: Output resource pressure is counted without clearing inputs
The compositor SHALL handle temporary output buffer pressure by dropping or skipping the current output tick without releasing retained input frames.

#### Scenario: Mosaic output buffer busy
- **WHEN** the compositor cannot obtain an output buffer for the current tick
- **THEN** it records a busy/drop statistic and keeps retained input frames available for a future tick

### Requirement: Pacing telemetry is observable
The system SHALL expose runtime telemetry sufficient to diagnose pacing behavior.

#### Scenario: Health log includes pacing counters
- **WHEN** mosaic health statistics are logged
- **THEN** the log includes submit rate, tick rate, compose rate, encoder push rate, busy/drop count, and latest frame identifiers for the four channels
