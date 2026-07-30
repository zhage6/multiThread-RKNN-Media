#pragma once

#include "StreamPublisher.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// 默认端口为 8554，可通过环境变量 ZLMEDIAKIT_RTSP_PORT 修改。
std::string MakeEmbeddedRtspUrl(const std::string& stream);

// 把编码后的 H.264 注册为进程内 ZLMediaKit 媒体源。
// Init 中的 URL 只用于确定 RTSP 监听端口、app 和 stream：
// rtsp://host:port/app/stream
class ZlMediaPublisher final : public StreamPublisher
{
public:
    ZlMediaPublisher() = default;
    ~ZlMediaPublisher() override;

    ZlMediaPublisher(const ZlMediaPublisher&) = delete;
    ZlMediaPublisher& operator=(const ZlMediaPublisher&) = delete;

    bool Init(const std::string& url,
              int width,
              int height,
              int fps,
              const uint8_t* extra_data,
              size_t extra_size) override;
    bool Push(const EncodedPacket& packet) override;
    void Close() override;

private:
    bool InputAnnexB(const uint8_t* data,
                     size_t size,
                     uint64_t dts_ms,
                     uint64_t pts_ms);

    std::mutex mutex_;
    void* media_ = nullptr;
    std::string url_;
    bool started_ = false;
    bool warned_bad_annexb_ = false;
    uint64_t pushed_packets_ = 0;
    uint64_t stats_last_pushed_packets_ = 0;
    long long max_input_us_ = 0;
    int64_t first_packet_pts_ = 0;
    std::chrono::steady_clock::time_point stats_last_;
    std::chrono::steady_clock::time_point first_packet_wall_;
};
