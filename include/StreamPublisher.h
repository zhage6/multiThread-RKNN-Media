#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>
#include <chrono>
#include "TimingLogger.h"

struct EncodedPacket 
{
    int channel_id = -1;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;
    int64_t pts = 0;
    int64_t dts = 0;
};

class StreamPublisher 
{
public:
    virtual ~StreamPublisher() = default;
    virtual bool Init(const std::string& url, int width, int height, int fps, const uint8_t* extra_data, size_t extra_size) = 0;
    virtual bool Push(const EncodedPacket& packet) = 0;
    virtual void Close() = 0;
};//多态接口，方便未来扩展网络推流等功能

class FilePublisher : public StreamPublisher 
{
public:
    FilePublisher() = default;
    ~FilePublisher() override { Close(); }

    bool Init(const std::string& url, int width, int height, int fps,const uint8_t* extra_data, size_t extra_size) override
    {
        (void)width;
        (void)height;//(void)是为了避免未使用参数的编译警告，因为这个简单的文件发布器不需要这些参数，但未来如果扩展网络推流可能会用到它们，所以保留接口一致性)
        (void)fps;
        (void)extra_data;
        (void)extra_size;
        fp_ = std::fopen(url.c_str(), "wb");
        return fp_ != nullptr;
    }

    bool Push(const EncodedPacket& packet) override
    {
        if (fp_ == nullptr || packet.data == nullptr || packet.size == 0) 
        {
            return false;
        }
        return std::fwrite(packet.data, 1, packet.size, fp_) == packet.size;
    }

    void Close() override
    {
        if (fp_) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
    }

private:
    FILE* fp_ = nullptr;
};

class RtspPublisher : public StreamPublisher 
{
public:
    RtspPublisher() = default;
    ~RtspPublisher() override { Close(); }

    bool Init(const std::string& url, int width, int height, int fps,const uint8_t* extra_data, size_t extra_size) override
    {
        url_ = url;
        width_ = width;
        height_ = height;
        fps_ = fps;
        stats_start_ = std::chrono::steady_clock::now();
        stats_last_ = stats_start_;
        first_packet_seen_ = false;
        pushed_packets_ = 0;
        stats_last_pushed_packets_ = 0;
        max_write_ms_ = 0;

        avformat_network_init();

        int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "rtsp", url.c_str());
        if (ret < 0 || fmt_ctx_ == nullptr) {
            PrintAvError("avformat_alloc_output_context2 failed", ret);
            return false;
        }
        printf("RTSP init 1 alloc ctx\n");

        stream_ = avformat_new_stream(fmt_ctx_, nullptr);
        if (stream_ == nullptr) {
            printf("avformat_new_stream failed\n");
            Close();
            return false;
        }
        printf("RTSP init 2 new stream\n");

        stream_->time_base = AVRational{1, 90000};

        AVCodecParameters* codecpar = stream_->codecpar;
        codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        codecpar->codec_id = AV_CODEC_ID_H264;
        codecpar->width = width_;
        codecpar->height = height_;
        codecpar->format = AV_PIX_FMT_YUV420P;
        codecpar->codec_tag = 0;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "2000000", 0);
        av_dict_set(&options, "rw_timeout", "2000000", 0);

        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) 
        {
            ret = avio_open2(&fmt_ctx_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
            if (ret < 0) {
                PrintAvError("avio_open2 failed", ret);
                av_dict_free(&options);
                Close();
                return false;
            }
        }
        printf("RTSP init 3 open io\n");
        if (extra_data && extra_size > 0) 
        {
            codecpar->extradata = (uint8_t*)av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
            std::memcpy(codecpar->extradata, extra_data, extra_size);
            codecpar->extradata_size = extra_size;
        }
        ret = avformat_write_header(fmt_ctx_, &options);
        printf("RTSP init 4 write header\n");
        av_dict_free(&options);

        if (ret < 0) {
            PrintAvError("avformat_write_header failed", ret);
            Close();
            return false;
        }

        started_ = true;
        printf("RTSP publisher started: %s\n", url_.c_str());
        return true;
    }

    bool Push(const EncodedPacket& packet) override
    {
        if (!started_ || fmt_ctx_ == nullptr || stream_ == nullptr ||
            packet.data == nullptr || packet.size == 0) {
            return false;
        }

        AVPacket* avpkt = av_packet_alloc();
        if (avpkt == nullptr) {
            return false;
        }

        int ret = av_new_packet(avpkt, static_cast<int>(packet.size));
        if (ret < 0) {
            av_packet_free(&avpkt);
            PrintAvError("av_new_packet failed", ret);
            return false;
        }

        std::memcpy(avpkt->data, packet.data, packet.size);

        avpkt->stream_index = stream_->index;
        avpkt->pts = packet.pts;
        avpkt->dts = packet.dts;
        avpkt->duration = av_rescale_q(1, AVRational{1, fps_}, stream_->time_base);
        avpkt->pos = -1;

        if (packet.keyframe) 
        {
            avpkt->flags |= AV_PKT_FLAG_KEY;
        }

        auto write_start = std::chrono::steady_clock::now();
        ret = av_interleaved_write_frame(fmt_ctx_, avpkt);
        auto write_end = std::chrono::steady_clock::now();
        long long write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            write_end - write_start).count();

        av_packet_free(&avpkt);

        if (ret < 0) {
            PrintAvError("av_interleaved_write_frame failed", ret);
            return false;
        }

        pushed_packets_++;
        if (write_ms > max_write_ms_) {
            max_write_ms_ = write_ms;
        }

        if (!first_packet_seen_) {
            first_packet_seen_ = true;
            first_packet_wall_ = write_end;
            first_packet_pts_ = packet.pts;
        }

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            write_end - stats_last_).count();
        if (elapsed_ms >= 1000) {
            double seconds = elapsed_ms / 1000.0;
            auto wall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                write_end - first_packet_wall_).count();
            long long media_elapsed_ms =
                static_cast<long long>((packet.pts - first_packet_pts_) * 1000 / 90000);
            long long media_wall_diff_ms = media_elapsed_ms - wall_elapsed_ms;

            timing::Log("rtsp_health url=%s push_fps=%.2f write_ms_last=%lld write_ms_max=%lld media_wall_diff_ms=%lld total_packets=%llu pts=%lld size=%zu key=%d",
                        url_.c_str(),
                        (pushed_packets_ - stats_last_pushed_packets_) / seconds,
                        write_ms,
                        max_write_ms_,
                        media_wall_diff_ms,
                        static_cast<unsigned long long>(pushed_packets_),
                        static_cast<long long>(packet.pts),
                        packet.size,
                        packet.keyframe ? 1 : 0);

            stats_last_ = write_end;
            stats_last_pushed_packets_ = pushed_packets_;
            max_write_ms_ = 0;
        }

        return true;
    }

    void Close() override
    {
        if (fmt_ctx_) {
            if (started_) {
                av_write_trailer(fmt_ctx_);
            }

            if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE) && fmt_ctx_->pb) {
                avio_closep(&fmt_ctx_->pb);
            }

            avformat_free_context(fmt_ctx_);
            fmt_ctx_ = nullptr;
        }

        stream_ = nullptr;
        started_ = false;
    }

private:
    static void PrintAvError(const char* prefix, int err)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(err, errbuf, sizeof(errbuf));
        printf("%s: %s (%d)\n", prefix, errbuf, err);
    }

    std::string url_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    bool started_ = false;
    bool first_packet_seen_ = false;
    uint64_t pushed_packets_ = 0;
    uint64_t stats_last_pushed_packets_ = 0;
    long long max_write_ms_ = 0;
    int64_t first_packet_pts_ = 0;
    std::chrono::steady_clock::time_point stats_start_;
    std::chrono::steady_clock::time_point stats_last_;
    std::chrono::steady_clock::time_point first_packet_wall_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* stream_ = nullptr;
};
