#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
};
