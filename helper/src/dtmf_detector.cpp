#include "dtmf_detector.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ivrdroid {
namespace {

constexpr std::array<double, 4> kLowFrequencies {697.0, 770.0, 852.0, 941.0};
constexpr std::array<double, 4> kHighFrequencies {1209.0, 1336.0, 1477.0, 1633.0};
constexpr std::array<std::array<char, 4>, 4> kDigits {{
    {{'1', '2', '3', 'A'}},
    {{'4', '5', '6', 'B'}},
    {{'7', '8', '9', 'C'}},
    {{'*', '0', '#', 'D'}},
}};

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumRmsDbfs = -48.0;
constexpr int kMinimumPeak = 100;
constexpr double kMinimumLowShare = 0.04;
constexpr double kMinimumHighShare = 0.04;
constexpr double kMinimumCombinedShare = 0.16;
constexpr double kMinimumDominance = 12.0;
constexpr double kMinimumTwist = 0.15;
constexpr double kMaximumTwist = 6.5;
constexpr int kStableFramesRequired = 2;
constexpr int kQuietFramesRequired = 2;

double EstimatedAmplitudeSquared(
    const int16_t* samples,
    size_t frameCount,
    size_t channel,
    double mean,
    double frequency,
    unsigned int sampleRate) {
    const double coefficient =
        2.0 * std::cos(2.0 * kPi * frequency / static_cast<double>(sampleRate));
    double state1 = 0.0;
    double state2 = 0.0;

    for (size_t index = 0; index < frameCount; ++index) {
        const double window =
            0.5 -
            0.5 * std::cos(
                2.0 * kPi * static_cast<double>(index) /
                static_cast<double>(std::max<size_t>(1, frameCount - 1)));
        const double state0 =
            (static_cast<double>(samples[index * 2 + channel]) - mean) * window +
            coefficient * state1 -
            state2;
        state2 = state1;
        state1 = state0;
    }

    const double power =
        state1 * state1 +
        state2 * state2 -
        coefficient * state1 * state2;
    const double frames = static_cast<double>(frameCount);
    return 16.0 * std::max(0.0, power) / (frames * frames);
}

template <size_t Size>
size_t MaximumIndex(const std::array<double, Size>& values) {
    return static_cast<size_t>(
        std::distance(values.begin(), std::max_element(values.begin(), values.end())));
}

template <size_t Size>
double SecondLargest(const std::array<double, Size>& values, size_t excluded) {
    double result = 0.0;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != excluded) result = std::max(result, values[index]);
    }
    return result;
}

}  // namespace

StereoDtmfDetector::StereoDtmfDetector(unsigned int sampleRate)
    : sampleRate_(sampleRate) {
}

DtmfFrameAnalysis StereoDtmfDetector::AnalyzeChannel(
    const int16_t* samples,
    size_t frameCount,
    size_t channel) const {
    DtmfFrameAnalysis analysis {};
    if (samples == nullptr || frameCount < 320 || channel > 1) return analysis;

    double mean = 0.0;
    int peak = 0;
    for (size_t index = 0; index < frameCount; ++index) {
        const int value = samples[index * 2 + channel];
        mean += static_cast<double>(value);
        peak = std::max(peak, std::abs(value));
    }
    mean /= static_cast<double>(frameCount);

    double sumSquares = 0.0;
    for (size_t index = 0; index < frameCount; ++index) {
        const double centered =
            static_cast<double>(samples[index * 2 + channel]) - mean;
        sumSquares += centered * centered;
    }
    const double rmsSquared = sumSquares / static_cast<double>(frameCount);
    const double rms = std::sqrt(rmsSquared);
    analysis.rmsDbfs =
        rms <= 0.0 ? -120.0 : 20.0 * std::log10(rms / 32768.0);
    analysis.peak = peak;
    if (analysis.rmsDbfs < kMinimumRmsDbfs || peak < kMinimumPeak) {
        return analysis;
    }

    std::array<double, 4> lowPowers {};
    std::array<double, 4> highPowers {};
    for (size_t index = 0; index < lowPowers.size(); ++index) {
        lowPowers[index] = EstimatedAmplitudeSquared(
            samples,
            frameCount,
            channel,
            mean,
            kLowFrequencies[index],
            sampleRate_);
        highPowers[index] = EstimatedAmplitudeSquared(
            samples,
            frameCount,
            channel,
            mean,
            kHighFrequencies[index],
            sampleRate_);
    }

    const size_t lowIndex = MaximumIndex(lowPowers);
    const size_t highIndex = MaximumIndex(highPowers);
    const double lowBest = lowPowers[lowIndex];
    const double highBest = highPowers[highIndex];
    const double lowSecond = SecondLargest(lowPowers, lowIndex);
    const double highSecond = SecondLargest(highPowers, highIndex);
    analysis.lowShare = lowBest / std::max(1.0, 2.0 * rmsSquared);
    analysis.highShare = highBest / std::max(1.0, 2.0 * rmsSquared);
    const double combinedShare = analysis.lowShare + analysis.highShare;
    const double lowDominance = lowBest / std::max(1.0, lowSecond);
    const double highDominance = highBest / std::max(1.0, highSecond);
    analysis.dominance = std::min(lowDominance, highDominance);
    analysis.twist = std::sqrt(lowBest / std::max(1.0, highBest));

    const bool valid =
        analysis.lowShare >= kMinimumLowShare &&
        analysis.highShare >= kMinimumHighShare &&
        combinedShare >= kMinimumCombinedShare &&
        lowDominance >= kMinimumDominance &&
        highDominance >= kMinimumDominance &&
        analysis.twist >= kMinimumTwist &&
        analysis.twist <= kMaximumTwist;
    analysis.confidence = std::min(
        1.0,
        std::min(combinedShare, 1.0) *
            std::min(lowDominance / 3.0, 1.0) *
            std::min(highDominance / 3.0, 1.0));
    if (valid) analysis.candidate = kDigits[lowIndex][highIndex];
    return analysis;
}

char StereoDtmfDetector::ProcessFrame(
    const int16_t* samples,
    size_t frameCount,
    DtmfFrameAnalysis* leftAnalysis,
    DtmfFrameAnalysis* rightAnalysis) {
    const DtmfFrameAnalysis left = AnalyzeChannel(samples, frameCount, 0);
    const DtmfFrameAnalysis right = AnalyzeChannel(samples, frameCount, 1);
    if (leftAnalysis != nullptr) *leftAnalysis = left;
    if (rightAnalysis != nullptr) *rightAnalysis = right;

    const char candidate =
        left.candidate != 0 && left.candidate == right.candidate
            ? left.candidate
            : 0;
    if (candidate == 0) {
        pending_ = 0;
        pendingFrames_ = 0;
        ++quietFrames_;
        if (quietFrames_ >= kQuietFramesRequired) emitted_ = 0;
        return 0;
    }

    quietFrames_ = 0;
    if (candidate == pending_) {
        ++pendingFrames_;
    } else {
        pending_ = candidate;
        pendingFrames_ = 1;
    }

    if (pendingFrames_ >= kStableFramesRequired && emitted_ != candidate) {
        emitted_ = candidate;
        return candidate;
    }
    return 0;
}

void StereoDtmfDetector::Reset() {
    pending_ = 0;
    pendingFrames_ = 0;
    emitted_ = 0;
    quietFrames_ = 0;
}

}  // namespace ivrdroid
