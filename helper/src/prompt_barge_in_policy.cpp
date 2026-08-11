#include "prompt_barge_in_policy.h"

#include <algorithm>

namespace ivrdroid {

PromptBargeInAttemptPolicy::PromptBargeInAttemptPolicy(
    std::string_view configuredDigits,
    uint64_t postPromptTimeoutFrames)
    : configuredDigits_(configuredDigits),
      postPromptTimeoutFrames_(std::max<uint64_t>(1, postPromptTimeoutFrames)) {}

bool PromptBargeInAttemptPolicy::promptPlaying() const {
    return promptPlaying_;
}

PromptDigitDecision PromptBargeInAttemptPolicy::ObserveDigit(char digit) const {
    if (!promptPlaying_ || configuredDigits_.find(digit) != std::string_view::npos) {
        return PromptDigitDecision::Accept;
    }
    return PromptDigitDecision::IgnoreUntilPromptCompletes;
}

void PromptBargeInAttemptPolicy::PromptCompleted() {
    promptPlaying_ = false;
    postPromptFrames_ = 0;
}

bool PromptBargeInAttemptPolicy::AdvancePostPromptFrame() {
    if (promptPlaying_) return false;
    ++postPromptFrames_;
    return postPromptFrames_ >= postPromptTimeoutFrames_;
}

}  // namespace ivrdroid
