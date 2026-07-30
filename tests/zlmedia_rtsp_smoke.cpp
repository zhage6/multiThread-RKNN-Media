#include "ZlMediaPublisher.h"

extern "C" {
#include "mk_h264_splitter.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{
struct SmokeContext
{
    ZlMediaPublisher* publisher = nullptr;
    int fps = 24;
    int max_frames = 240;
    int frames = 0;
    bool push_failed = false;
};

bool ContainsIdr(const char* data, int size)
{
    for (int i = 0; i + 4 < size; ++i) {
        int prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            prefix = 3;
        } else if (data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            prefix = 4;
        }
        if (prefix != 0 && i + prefix < size &&
            (static_cast<unsigned char>(data[i + prefix]) & 0x1f) == 5) {
            return true;
        }
    }
    return false;
}

void OnH264AccessUnit(void* user_data,
                      mk_h264_splitter,
                      const char* data,
                      int size)
{
    SmokeContext* context = static_cast<SmokeContext*>(user_data);
    if (!context || !context->publisher || size <= 0 ||
        context->frames >= context->max_frames) {
        return;
    }

    EncodedPacket packet;
    packet.data = reinterpret_cast<const uint8_t*>(data);
    packet.size = static_cast<size_t>(size);
    packet.keyframe = ContainsIdr(data, size);
    packet.pts =
        static_cast<int64_t>(context->frames) * 90000 / context->fps;
    packet.dts = packet.pts;

    if (!context->publisher->Push(packet)) {
        context->push_failed = true;
        return;
    }
    ++context->frames;
    std::this_thread::sleep_for(
        std::chrono::microseconds(1000000 / context->fps));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: %s <annex-b.h264> [frame-count]\n", argv[0]);
        return 2;
    }

    FILE* input = std::fopen(argv[1], "rb");
    if (!input) {
        std::perror("fopen");
        return 2;
    }

    SmokeContext context;
    if (argc == 3) {
        context.max_frames = std::max(1, std::atoi(argv[2]));
    }

    ZlMediaPublisher publisher;
    context.publisher = &publisher;
    if (!publisher.Init(
            "rtsp://127.0.0.1:18554/live/smoke",
            1920,
            1080,
            context.fps,
            nullptr,
            0)) {
        std::fclose(input);
        return 1;
    }

    mk_h264_splitter splitter =
        mk_h264_splitter_create(OnH264AccessUnit, &context, 0);
    char buffer[64 * 1024];
    while (context.frames < context.max_frames && !context.push_failed) {
        const size_t size = std::fread(buffer, 1, sizeof(buffer), input);
        if (size == 0) {
            break;
        }
        mk_h264_splitter_input_data(
            splitter, buffer, static_cast<int>(size));
    }

    mk_h264_splitter_release(splitter);
    std::fclose(input);

    if (context.push_failed || context.frames == 0) {
        std::fprintf(stderr, "RTSP smoke source failed after %d frames\n",
                     context.frames);
        return 1;
    }

    std::printf("RTSP smoke source completed %d frames\n", context.frames);
    return 0;
}
