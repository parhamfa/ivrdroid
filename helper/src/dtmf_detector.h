#pragma once

#include <cstddef>
#include <cstdint>

namespace ivrdroid {

struct DtmfFrameAnalysis {
    char candidate = 0;
    double confidence = 0.0;
    double rmsDbfs = -120.0;
    int peak = 0;
    double lowShare = 0.0;
    double highShare = 0.0;
    double dominance = 0.0;
    double twist = 0.0;
};

class StereoDtmfDetector {
public:
    explicit StereoDtmfDetector(unsigned int sampleRate);

    char ProcessFrame(
        const int16_t* interleavedSamples,
        size_t frameCount,
        DtmfFrameAnalysis* leftAnalysis = nullptr,
        DtmfFrameAnalysis* rightAnalysis = nullptr);

    void Reset();

private:
    DtmfFrameAnalysis AnalyzeChannel(
        const int16_t* interleavedSamples,
        size_t frameCount,
        size_t channel) const;

    unsigned int sampleRate_;
    char pending_ = 0;
    int pendingFrames_ = 0;
    char emitted_ = 0;
    int quietFrames_ = 0;
};

}  // namespace ivrdroid
