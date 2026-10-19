#pragma once

#include <new>

#include "engine/ipu/hle/ffmpeg_video_decoder.h"

// Exercise a real resource-construction failure while MpegApi copies its
// decoder factory into an incoming context. The video algorithm is unchanged.
class MovieCheckpointFactory {
public:
    MovieCheckpointFactory(bool& fail, unsigned& copies, unsigned& live)
        : fail_(&fail), copies_(&copies), live_(&live) { ++*live_; }
    MovieCheckpointFactory(const MovieCheckpointFactory& from)
        : fail_(from.fail_), copies_(from.copies_), live_(from.live_) {
        ++*copies_;
        if (*fail_) throw std::bad_alloc();
        ++*live_;
    }
    ~MovieCheckpointFactory() { --*live_; }
    std::unique_ptr<ps2r::ipu::VideoDecoder> operator()() const {
        return std::make_unique<ps2r::ipu::FfmpegVideoDecoder>();
    }
private:
    bool* fail_;
    unsigned* copies_;
    unsigned* live_;
};
