#include "sha256.h"

#include <cassert>
#include <string>

int main() {
    assert(ivrdroid::Sha256Hex("", 0) ==
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");
    const std::string value = "abc";
    assert(ivrdroid::Sha256Hex(value.data(), value.size()) ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    assert(ivrdroid::IsLowerHexSha256(std::string(64, 'a')));
    assert(!ivrdroid::IsLowerHexSha256(std::string(64, 'A')));
    return 0;
}
