#include "menu_policy.h"

#include <cassert>
#include <iostream>

namespace {

void RoutesValidDigits() {
    {
        ivrdroid::MenuPolicy policy;
        const auto result = policy.Handle({ivrdroid::MenuInputKind::Digit, '1'});
        assert(result.kind == ivrdroid::MenuDecisionKind::PlayTerminal);
        assert(result.prompt == ivrdroid::MenuPrompt::Sales);
    }
    {
        ivrdroid::MenuPolicy policy;
        const auto result = policy.Handle({ivrdroid::MenuInputKind::Digit, '2'});
        assert(result.kind == ivrdroid::MenuDecisionKind::PlayTerminal);
        assert(result.prompt == ivrdroid::MenuPrompt::Support);
    }
    {
        ivrdroid::MenuPolicy policy;
        const auto result = policy.Handle({ivrdroid::MenuInputKind::Digit, '0'});
        assert(result.kind == ivrdroid::MenuDecisionKind::PlayTerminal);
        assert(result.prompt == ivrdroid::MenuPrompt::Operator);
    }
}

void RetriesTwiceThenFinishes() {
    ivrdroid::MenuPolicy policy;
    assert(
        policy.Handle({ivrdroid::MenuInputKind::Digit, '9'}).kind ==
        ivrdroid::MenuDecisionKind::Retry);
    assert(
        policy.Handle({ivrdroid::MenuInputKind::Timeout, 0}).kind ==
        ivrdroid::MenuDecisionKind::Retry);
    assert(
        policy.Handle({ivrdroid::MenuInputKind::Digit, '#'}).kind ==
        ivrdroid::MenuDecisionKind::Finish);
    assert(policy.retryCount() == 3);
}

}  // namespace

int main() {
    RoutesValidDigits();
    RetriesTwiceThenFinishes();
    std::cout << "Menu policy tests passed." << std::endl;
    return 0;
}
