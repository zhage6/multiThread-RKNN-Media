#include "FfmpegH264Source.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <cstdio>
#include <utility>

namespace {

void PrintAvError(const char* action, int error)
{
    char error_text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(error, error_text, sizeof(error_text));
    std::printf("FFmpeg %s failed: %s (%d)\n", action, error_text, error);
}

bool IsRtspUrl(const std::string& url)
{
    return url.rfind("rtsp://", 0) == 0;
}

int64_t PacketPtsUs(const AVPacket* packet, AVRational time_base)
{
    int64_t timestamp = packet->pts;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = packet->dts;
    }
    if (timestamp == AV_NOPTS_VALUE) {
        return -1;
    }

    return av_rescale_q(timestamp, time_base, AVRational{1, 1000000});
}

}  // namespace

struct FfmpegH264Source::Impl {
    AVFormatContext* format_ctx = nullptr;
    AVBSFContext* bitstream_filter = nullptr;
    AVPacket* input_packet = nullptr;
    AVPacket* output_packet = nullptr;
    int video_stream_index = -1;
    AVRational video_time_base{0, 1};
};

FfmpegH264Source::FfmpegH264Source()
    : impl_(new Impl())
{
}

FfmpegH264Source::~FfmpegH264Source()
{
    Close();
}

bool FfmpegH264Source::Open(const std::string& url)
{
    Close();
    avformat_network_init();

    AVDictionary* options = nullptr;
    if (IsRtspUrl(url)) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    // A finite timeout lets VideoChannel stop and reconnect instead of being
    // trapped forever in av_read_frame() after a camera disconnects.
    av_dict_set(&options, "stimeout", "2000000", 0);
    av_dict_set(&options, "rw_timeout", "2000000", 0);

    int ret = avformat_open_input(&impl_->format_ctx, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        PrintAvError("open input", ret);
        Close();
        return false;
    }

    ret = avformat_find_stream_info(impl_->format_ctx, nullptr);
    if (ret < 0) {
        PrintAvError("find stream info", ret);
        Close();
        return false;
    }

    ret = av_find_best_stream(impl_->format_ctx,
                              AVMEDIA_TYPE_VIDEO,
                              -1,
                              -1,
                              nullptr,
                              0);
    if (ret < 0) {
        PrintAvError("find H.264 video stream", ret);
        Close();
        return false;
    }

    impl_->video_stream_index = ret;
    AVStream* stream = impl_->format_ctx->streams[impl_->video_stream_index];
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        std::printf("FFmpeg input only supports H.264 for MPP AVC decoder, codec_id=%d\n",
                    stream->codecpar->codec_id);
        Close();
        return false;
    }
    impl_->video_time_base = stream->time_base;

    // Keep the MPP input uniform. h264_mp4toannexb is a no-op for an Annex-B
    // packet and converts length-prefixed H.264 when a source provides it.
    const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (filter == nullptr || av_bsf_alloc(filter, &impl_->bitstream_filter) < 0) {
        std::printf("FFmpeg cannot create h264_mp4toannexb filter\n");
        Close();
        return false;
    }
    ret = avcodec_parameters_copy(impl_->bitstream_filter->par_in, stream->codecpar);
    if (ret < 0) {
        PrintAvError("copy H.264 codec parameters", ret);
        Close();
        return false;
    }
    impl_->bitstream_filter->time_base_in = stream->time_base;
    ret = av_bsf_init(impl_->bitstream_filter);
    if (ret < 0) {
        PrintAvError("initialize h264_mp4toannexb filter", ret);
        Close();
        return false;
    }

    impl_->input_packet = av_packet_alloc();
    impl_->output_packet = av_packet_alloc();
    if (impl_->input_packet == nullptr || impl_->output_packet == nullptr) {
        std::printf("FFmpeg packet allocation failed\n");
        Close();
        return false;
    }

    std::printf("FFmpeg H.264 input opened: %s, stream=%d, time_base=%d/%d\n",
                url.c_str(),
                impl_->video_stream_index,
                stream->time_base.num,
                stream->time_base.den);
    return true;
}

FfmpegH264Source::ReadStatus FfmpegH264Source::ReadNext(const PacketCallback& callback)
{
    if (impl_->format_ctx == nullptr || impl_->bitstream_filter == nullptr ||
        impl_->input_packet == nullptr || impl_->output_packet == nullptr) {
        return ReadStatus::kError;
    }

    while (true) {
        int ret = av_bsf_receive_packet(impl_->bitstream_filter, impl_->output_packet);
        if (ret == 0) {
            const int64_t pts_us = PacketPtsUs(impl_->output_packet, impl_->video_time_base);
            const bool keep_reading = callback(impl_->output_packet->data,
                                               static_cast<size_t>(impl_->output_packet->size),
                                               pts_us);
            av_packet_unref(impl_->output_packet);
            return keep_reading ? ReadStatus::kPacket : ReadStatus::kStopped;
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            PrintAvError("receive filtered packet", ret);
            return ReadStatus::kError;
        }

        ret = av_read_frame(impl_->format_ctx, impl_->input_packet);
        if (ret == AVERROR(EAGAIN)) {
            return ReadStatus::kAgain;
        }
        if (ret == AVERROR_EOF) {
            return ReadStatus::kEnd;
        }
        if (ret < 0) {
            PrintAvError("read frame", ret);
            return ReadStatus::kError;
        }

        if (impl_->input_packet->stream_index != impl_->video_stream_index) {
            av_packet_unref(impl_->input_packet);
            continue;
        }

        ret = av_bsf_send_packet(impl_->bitstream_filter, impl_->input_packet);
        av_packet_unref(impl_->input_packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            PrintAvError("send packet to h264_mp4toannexb", ret);
            return ReadStatus::kError;
        }
    }
}

void FfmpegH264Source::Close()
{
    if (!impl_) {
        return;
    }
    av_packet_free(&impl_->input_packet);
    av_packet_free(&impl_->output_packet);
    av_bsf_free(&impl_->bitstream_filter);
    avformat_close_input(&impl_->format_ctx);
    impl_->video_stream_index = -1;
    impl_->video_time_base = AVRational{0, 1};
}
