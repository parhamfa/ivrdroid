#pragma once

namespace ivrdroid {

enum class MenuInputKind {
    Digit,
    Timeout,
};

struct MenuInput {
    MenuInputKind kind;
    char digit;
};

enum class MenuPrompt {
    Main,
    Sales,
    Support,
    Operator,
};

enum class MenuDecisionKind {
    PlayTerminal,
    Retry,
    Finish,
};

struct MenuDecision {
    MenuDecisionKind kind;
    MenuPrompt prompt;
};

class MenuPolicy {
public:
    static constexpr int kMaximumRetries = 2;

    MenuDecision Handle(MenuInput input);
    int retryCount() const;

private:
    int retries_ = 0;
};

}  // namespace ivrdroid
