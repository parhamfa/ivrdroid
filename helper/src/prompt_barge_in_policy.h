#pragma once

#include <cstdint>
#include <string_view>

namespace ivrdroid {

enum class PromptDigitDecision {
    Accept,
    IgnoreUntilPromptCompletes,
};

class PromptBargeInAttemptPolicy {
public:
    PromptBargeInAttemptPolicy(
        std::string_view configuredDigits,
        uint64_t postPromptTimeoutFrames);

    bool promptPlaying() const;
    PromptDigitDecision ObserveDigit(char digit) const;
    void PromptCompleted();
    bool AdvancePostPromptFrame();

private:
    std::string_view configuredDigits_;
    uint64_t postPromptTimeoutFrames_;
    uint64_t postPromptFrames_ = 0;
    bool promptPlaying_ = true;
};

}  // namespace ivrdroid
