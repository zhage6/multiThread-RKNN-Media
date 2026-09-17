#ifndef FFMPEG_H264_SOURCE_H
#define FFMPEG_H264_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// FFmpeg is used only for transport and demuxing. Decoding remains in MPP.
class FfmpegH264Source {
public:
    enum class ReadStatus {
        kPacket,
        kAgain,
        kEnd,
        kError,
        kStopped,
    };

    using PacketCallback = std::function<bool(const uint8_t* data,
                                              size_t size,
                                              int64_t pts_us)>;

    FfmpegH264Source();
    ~FfmpegH264Source();

    FfmpegH264Source(const FfmpegH264Source&) = delete;
    FfmpegH264Source& operator=(const FfmpegH264Source&) = delete;

    bool Open(const std::string& url);
    ReadStatus ReadNext(const PacketCallback& callback);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // FFMPEG_H264_SOURCE_H
