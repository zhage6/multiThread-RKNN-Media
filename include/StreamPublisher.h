#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
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
