#include "menu_policy.h"

namespace ivrdroid {

MenuDecision MenuPolicy::Handle(MenuInput input) {
    if (input.kind == MenuInputKind::Digit) {
        if (input.digit == '1') {
            return {MenuDecisionKind::PlayTerminal, MenuPrompt::Sales};
        }
        if (input.digit == '2') {
            return {MenuDecisionKind::PlayTerminal, MenuPrompt::Support};
        }
        if (input.digit == '0') {
            return {MenuDecisionKind::PlayTerminal, MenuPrompt::Operator};
        }
    }

    ++retries_;
    return retries_ > kMaximumRetries
        ? MenuDecision {MenuDecisionKind::Finish, MenuPrompt::Main}
        : MenuDecision {MenuDecisionKind::Retry, MenuPrompt::Main};
}

int MenuPolicy::retryCount() const {
    return retries_;
}

}  // namespace ivrdroid
