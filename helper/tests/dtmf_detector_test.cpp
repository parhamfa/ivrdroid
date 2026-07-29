#include "dtmf_detector.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr unsigned int kSampleRate = 48'000;
constexpr size_t kFrameCount = 1'200;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLow[] = {697.0, 770.0, 852.0, 941.0};
constexpr double kHigh[] = {1209.0, 1336.0, 1477.0, 1633.0};
constexpr char kDigits[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

std::pair<size_t, size_t> Indexes(char digit) {
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            if (kDigits[row][column] == digit) return {row, column};
        }
    }
    assert(false);
    return {};
}

std::vector<int16_t> Frame(
    char leftDigit,
    char rightDigit,
    double lowAmplitude,
    double highAmplitude,
    uint64_t absoluteFrame) {
    std::vector<int16_t> samples(kFrameCount * 2);
    const auto left = Indexes(leftDigit);
    const auto right = Indexes(rightDigit);
    for (size_t index = 0; index < kFrameCount; ++index) {
        const double time =
            static_cast<double>(absoluteFrame * kFrameCount + index) /
            static_cast<double>(kSampleRate);
        const double noise = 55.0 * std::sin(2.0 * kPi * 311.0 * time);
        samples[index * 2] = static_cast<int16_t>(std::lround(
            lowAmplitude * std::sin(2.0 * kPi * kLow[left.first] * time) +
            highAmplitude * std::sin(2.0 * kPi * kHigh[left.second] * time) +
            noise));
        samples[index * 2 + 1] = static_cast<int16_t>(std::lround(
            lowAmplitude * std::sin(2.0 * kPi * kLow[right.first] * time) +
            highAmplitude * std::sin(2.0 * kPi * kHigh[right.second] * time) +
            noise));
    }
    return samples;
}

std::vector<int16_t> Silence() {
    return std::vector<int16_t>(kFrameCount * 2);
}

std::string DetectSequence(
    const std::string& sequence,
    double lowAmplitude,
    double highAmplitude) {
    ivrdroid::StereoDtmfDetector detector(kSampleRate);
    std::string detected;
    uint64_t absoluteFrame = 0;
    for (char digit : sequence) {
        for (int frame = 0; frame < 5; ++frame) {
            const auto samples = Frame(
                digit,
                digit,
                lowAmplitude,
                highAmplitude,
                absoluteFrame++);
            const char result = detector.ProcessFrame(samples.data(), kFrameCount);
            if (result != 0) detected += result;
        }
        for (int frame = 0; frame < 3; ++frame) {
            const auto samples = Silence();
            const char result = detector.ProcessFrame(samples.data(), kFrameCount);
            if (result != 0) detected += result;
            ++absoluteFrame;
        }
    }
    return detected;
}

void RejectsShortAndMismatchedTones() {
    ivrdroid::StereoDtmfDetector detector(kSampleRate);
    auto samples = Frame('1', '1', 6500.0, 6200.0, 0);
    assert(detector.ProcessFrame(samples.data(), kFrameCount) == 0);

    detector.Reset();
    for (int frame = 0; frame < 5; ++frame) {
        samples = Frame('1', '2', 6500.0, 6200.0, static_cast<uint64_t>(frame));
        assert(detector.ProcessFrame(samples.data(), kFrameCount) == 0);
    }
}

void RejectsNonDtmfAudio() {
    ivrdroid::StereoDtmfDetector detector(kSampleRate);
    for (int frame = 0; frame < 20; ++frame) {
        std::vector<int16_t> samples(kFrameCount * 2);
        for (size_t index = 0; index < kFrameCount; ++index) {
            const double time =
                static_cast<double>(frame * kFrameCount + index) /
                static_cast<double>(kSampleRate);
            const int16_t value = static_cast<int16_t>(std::lround(
                7000.0 * std::sin(2.0 * kPi * 440.0 * time) +
                2300.0 * std::sin(2.0 * kPi * 880.0 * time)));
            samples[index * 2] = value;
            samples[index * 2 + 1] = value;
        }
        assert(detector.ProcessFrame(samples.data(), kFrameCount) == 0);
    }
}

}  // namespace

int main() {
    assert(DetectSequence("123456789*0#", 6500.0, 6200.0) == "123456789*0#");
    assert(DetectSequence("15920#", 6500.0, 1550.0) == "15920#");
    RejectsShortAndMismatchedTones();
    RejectsNonDtmfAudio();
    std::cout << "DTMF detector tests passed." << std::endl;
    return 0;
}
