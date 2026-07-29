#include "helper_protocol.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using ivrdroid::protocol::Command;
    assert(ivrdroid::protocol::ParseCommand("START_MENU\n") == Command::StartMenu);
    assert(ivrdroid::protocol::ParseCommand("START_MENU") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("PLAY_MAIN\n") == Command::Invalid);
    assert(ivrdroid::protocol::ParseCommand("START_MENU\nignored") == Command::Invalid);
    assert(
        ivrdroid::protocol::ParseCommand(std::string(65, 'A')) ==
        Command::Invalid);
    std::cout << "Helper protocol tests passed." << std::endl;
    return 0;
}
