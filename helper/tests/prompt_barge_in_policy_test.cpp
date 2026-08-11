#include "prompt_barge_in_policy.h"

#include <cassert>

int main() {
    ivrdroid::PromptBargeInAttemptPolicy first("1234", 3);
    assert(first.promptPlaying());
    assert(first.ObserveDigit('9') ==
        ivrdroid::PromptDigitDecision::IgnoreUntilPromptCompletes);
    assert(first.ObserveDigit('1') == ivrdroid::PromptDigitDecision::Accept);
    for (int frame = 0; frame < 100; ++frame) {
        assert(!first.AdvancePostPromptFrame());
    }
    first.PromptCompleted();
    assert(!first.promptPlaying());
    assert(first.ObserveDigit('9') == ivrdroid::PromptDigitDecision::Accept);
    assert(!first.AdvancePostPromptFrame());
    assert(!first.AdvancePostPromptFrame());
    assert(first.AdvancePostPromptFrame());

    // Every retry gets a fresh policy: its prompt is interruptible again and
    // its complete post-prompt timeout has not been consumed by the prior try.
    ivrdroid::PromptBargeInAttemptPolicy retry("1234", 3);
    assert(retry.promptPlaying());
    retry.PromptCompleted();
    assert(!retry.AdvancePostPromptFrame());
    return 0;
}
