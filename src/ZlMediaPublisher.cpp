#include "ZlMediaPublisher.h"

#include "TimingLogger.h"

extern "C" {
#include "mk_mediakit.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
struct RtspUrl
{
    uint16_t port = 554;
    std::string app;
    std::string stream;
};

bool ParsePort(const std::string& text, uint16_t* port)
{
    if (!port || text.empty()) {
        return false;
    }

    unsigned long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    if (value == 0) {
        return false;
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

bool ParseRtspUrl(const std::string& url, RtspUrl* parsed)
{
    if (!parsed || url.compare(0, 7, "rtsp://") != 0) {
        return false;
    }

    const size_t authority_begin = 7;
    const size_t path_begin = url.find('/', authority_begin);
    if (path_begin == std::string::npos || path_begin + 1 >= url.size()) {
        return false;
    }

    const std::string authority =
        url.substr(authority_begin, path_begin - authority_begin);
    if (authority.empty()) {
        return false;
    }

    if (authority.front() == '[') {
        const size_t bracket = authority.find(']');
        if (bracket == std::string::npos) {
            return false;
        }
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':' ||
                !ParsePort(authority.substr(bracket + 2), &parsed->port)) {
                return false;
            }
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos &&
            !ParsePort(authority.substr(colon + 1), &parsed->port)) {
            return false;
        }
    }

    std::string path = url.substr(path_begin + 1);
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos) {
        path.resize(query);
    }

    const size_t app_end = path.find('/');
    if (app_end == std::string::npos || app_end == 0 ||
        app_end + 1 >= path.size()) {
        return false;
    }

    parsed->app = path.substr(0, app_end);
    parsed->stream = path.substr(app_end + 1);
    return !parsed->stream.empty();
}

class ZlMediaRuntime
{
public:
    static ZlMediaRuntime& Instance()
    {
        static ZlMediaRuntime runtime;
        return runtime;
    }

    bool StartRtsp(uint16_t requested_port)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            mk_config config;
            std::memset(&config, 0, sizeof(config));
            config.thread_num = 0;
            config.log_level = 2;
            config.log_mask = LOG_CONSOLE;
            mk_env_init(&config);
            initialized_ = true;
        }

        if (rtsp_port_ != 0) {
            if (rtsp_port_ != requested_port) {
                std::printf(
                    "ZLMediaKit RTSP server already listens on %u, cannot also use %u\n",
                    rtsp_port_,
                    requested_port);
                return false;
            }
            return true;
        }

        rtsp_port_ = mk_rtsp_server_start(requested_port, 0);
        if (rtsp_port_ == 0) {
            std::printf(
                "ZLMediaKit RTSP server failed to listen on port %u "
                "(is another MediaServer already running?)\n",
                requested_port);
            return false;
        }

        std::printf("ZLMediaKit embedded RTSP server listening on 0.0.0.0:%u\n",
                    rtsp_port_);
        return true;
    }

    ~ZlMediaRuntime()
    {
        if (rtsp_port_ != 0) {
            mk_stop_all_server();
        }
    }

private:
    ZlMediaRuntime() = default;

    std::mutex mutex_;
    bool initialized_ = false;
    uint16_t rtsp_port_ = 0;
};

size_t FindStartCode(const uint8_t* data, size_t size, size_t from)
{
    if (!data || from >= size) {
        return std::string::npos;
    }

    for (size_t i = from; i + 2 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            return i;
        }
        if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            return i;
        }
    }
    return std::string::npos;
}
} // namespace

std::string MakeEmbeddedRtspUrl(const std::string& stream)
{
    const char* configured_port = std::getenv("ZLMEDIAKIT_RTSP_PORT");
    const char* port =
        configured_port && configured_port[0] ? configured_port : "8554";
    return "rtsp://127.0.0.1:" + std::string(port) + "/live/" + stream;
}

ZlMediaPublisher::~ZlMediaPublisher()
{
    Close();
}

bool ZlMediaPublisher::Init(const std::string& url,
                            int width,
                            int height,
                            int fps,
                            const uint8_t* extra_data,
                            size_t extra_size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return false;
    }

    RtspUrl parsed;
    if (!ParseRtspUrl(url, &parsed)) {
        std::printf(
            "Invalid embedded RTSP URL: %s (expected rtsp://host:port/app/stream)\n",
            url.c_str());
        return false;
    }
    if (width <= 0 || height <= 0 || fps <= 0) {
        std::printf("Invalid H264 video parameters: %dx%d @ %d fps\n",
                    width, height, fps);
        return false;
    }
    if (!ZlMediaRuntime::Instance().StartRtsp(parsed.port)) {
        return false;
    }

    mk_ini option = mk_ini_create();
    if (!option) {
        std::printf("ZLMediaKit mk_ini_create failed\n");
        return false;
    }
    mk_ini_set_option(option, "modify_stamp", "0");
    mk_ini_set_option(option, "enable_audio", "0");
    mk_ini_set_option(option, "add_mute_audio", "0");
    mk_ini_set_option(option, "enable_hls", "0");
    mk_ini_set_option(option, "enable_hls_fmp4", "0");
    mk_ini_set_option(option, "enable_mp4", "0");
    mk_ini_set_option(option, "enable_rtsp", "1");
    mk_ini_set_option(option, "enable_rtmp", "0");
    mk_ini_set_option(option, "enable_ts", "0");
    mk_ini_set_option(option, "enable_fmp4", "0");

    media_ = mk_media_create2(
        "__defaultVhost__", parsed.app.c_str(), parsed.stream.c_str(), 0, option);
    mk_ini_release(option);
    if (!media_) {
        std::printf("ZLMediaKit failed to create media source %s/%s\n",
                    parsed.app.c_str(), parsed.stream.c_str());
        return false;
    }

    codec_args video_args;
    std::memset(&video_args, 0, sizeof(video_args));
    video_args.video.width = width;
    video_args.video.height = height;
    video_args.video.fps = fps;
    mk_track track = mk_track_create(MKCodecH264, &video_args);
    if (!track) {
        std::printf("ZLMediaKit failed to create H264 track\n");
        mk_media_release(static_cast<mk_media>(media_));
        media_ = nullptr;
        return false;
    }

    mk_media_init_track(static_cast<mk_media>(media_), track);
    mk_media_init_complete(static_cast<mk_media>(media_));
    mk_track_unref(track);

    url_ = url;
    started_ = true;
    warned_bad_annexb_ = false;
    pushed_packets_ = 0;
    stats_last_pushed_packets_ = 0;
    max_input_us_ = 0;
    first_packet_pts_ = 0;
    stats_last_ = std::chrono::steady_clock::now();
    first_packet_wall_ = stats_last_;

    if (extra_data && extra_size > 0 &&
        !InputAnnexB(extra_data, extra_size, 0, 0)) {
        std::printf("ZLMediaKit failed to input initial H264 SPS/PPS\n");
        mk_media_release(static_cast<mk_media>(media_));
        media_ = nullptr;
        started_ = false;
        return false;
    }

    std::printf("ZLMediaKit media source ready: %s\n", url_.c_str());
    std::printf("Remote clients should replace 127.0.0.1 with this board's IP address\n");
    return true;
}

bool ZlMediaPublisher::Push(const EncodedPacket& packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || !media_ || !packet.data || packet.size == 0) {
        return false;
    }

    const uint64_t dts_ms =
        static_cast<uint64_t>(std::max<int64_t>(0, packet.dts) / 90);
    const uint64_t pts_ms =
        static_cast<uint64_t>(std::max<int64_t>(0, packet.pts) / 90);

    const auto input_start = std::chrono::steady_clock::now();
    const bool ok = InputAnnexB(packet.data, packet.size, dts_ms, pts_ms);
    const auto input_end = std::chrono::steady_clock::now();
    const long long input_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            input_end - input_start).count();
    max_input_us_ = std::max(max_input_us_, input_us);

    if (!ok) {
        return false;
    }

    ++pushed_packets_;
    if (pushed_packets_ == 1) {
        first_packet_wall_ = input_end;
        first_packet_pts_ = packet.pts;
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            input_end - stats_last_).count();
    if (elapsed_ms >= 1000) {
        const double seconds = elapsed_ms / 1000.0;
        const auto wall_elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                input_end - first_packet_wall_).count();
        const int64_t media_elapsed_ms =
            (packet.pts - first_packet_pts_) / 90;

        timing::Log(
            "zl_rtsp_health url=%s input_fps=%.2f input_us_last=%lld "
            "input_us_max=%lld media_wall_diff_ms=%lld total_packets=%llu "
            "pts=%lld size=%zu key=%d",
            url_.c_str(),
            (pushed_packets_ - stats_last_pushed_packets_) / seconds,
            input_us,
            max_input_us_,
            static_cast<long long>(media_elapsed_ms - wall_elapsed_ms),
            static_cast<unsigned long long>(pushed_packets_),
            static_cast<long long>(packet.pts),
            packet.size,
            packet.keyframe ? 1 : 0);

        stats_last_ = input_end;
        stats_last_pushed_packets_ = pushed_packets_;
        max_input_us_ = 0;
    }

    return true;
}

void ZlMediaPublisher::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (media_) {
        mk_media_release(static_cast<mk_media>(media_));
        media_ = nullptr;
    }
    started_ = false;
}

bool ZlMediaPublisher::InputAnnexB(const uint8_t* data,
                                   size_t size,
                                   uint64_t dts_ms,
                                   uint64_t pts_ms)
{
    size_t current = FindStartCode(data, size, 0);
    if (current == std::string::npos) {
        if (!warned_bad_annexb_) {
            std::printf(
                "ZLMediaKit expected Annex-B H264 data, but no start code was found\n");
            warned_bad_annexb_ = true;
        }
        return false;
    }

    bool input_any = false;
    while (current != std::string::npos) {
        const size_t next = FindStartCode(data, size, current + 3);
        const size_t nal_end = next == std::string::npos ? size : next;
        if (nal_end > current + 3) {
            mk_frame frame = mk_frame_create(
                MKCodecH264,
                dts_ms,
                pts_ms,
                reinterpret_cast<const char*>(data + current),
                nal_end - current,
                nullptr,
                nullptr);
            if (!frame) {
                return false;
            }

            const int ret =
                mk_media_input_frame(static_cast<mk_media>(media_), frame);
            mk_frame_unref(frame);
            if (!ret) {
                return false;
            }
            input_any = true;
        }
        current = next;
    }
    return input_any;
}
