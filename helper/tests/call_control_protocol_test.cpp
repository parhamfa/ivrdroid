#include "call_control_protocol.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr char kSession[] = "11111111-1111-4111-8111-111111111111";
constexpr char kBlock[] = "22222222-2222-4222-8222-222222222222";
constexpr char kBoot[] = "33333333-3333-4333-8333-333333333333";

std::string Dial() {
    return std::string("IVRDROID_CALL_CONTROL_V2 DIAL ") + kSession +
        " 17 " + kBlock + " 1 " + kBoot +
        " 12345 +982112345678 30000\n";
}

std::string StatusWire(const char* phase, uint64_t sequence, const char* reason = "-") {
    return std::string("IVRDROID_CALL_CONTROL_V2 ") + phase + " " + kSession +
        " 17 " + kBlock + " " + std::to_string(sequence) + " " + kBoot +
        " 42000 " + reason + "\n";
}

}  // namespace

std::string ReadFixture(const char* path) {
    std::ifstream input(path);
    std::ostringstream value;
    value << input.rdbuf();
    assert(input.good() || input.eof());
    return value.str();
}

int main(int argc, char** argv) {
    using namespace ivrdroid::call_control;

    const Request dial = ParseRequest(Dial());
    assert(dial.kind == RequestKind::Dial);
    assert(dial.sessionUuid == kSession);
    assert(dial.revisionId == 17);
    assert(dial.blockUuid == kBlock);
    assert(dial.sequence == 1);
    assert(dial.elapsedMilliseconds == 12'345);
    assert(dial.phoneNumber == "+982112345678");
    assert(dial.answerTimeoutMilliseconds == 30'000);
    assert(dial.bootUuid == kBoot);
    assert(EncodeRequest(dial) == Dial());

    Request ready = dial;
    ready.kind = RequestKind::RecorderReady;
    ready.phoneNumber.clear();
    ready.answerTimeoutMilliseconds = 0;
    ready.sequence = 2;
    ready.elapsedMilliseconds = 12'400;
    const std::string readyWire = std::string(
        "IVRDROID_CALL_CONTROL_V2 RECORDER_READY ") + kSession + " 17 " +
        kBlock + " 2 " + kBoot + " 12400\n";
    assert(EncodeRequest(ready) == readyWire);
    assert(ParseRequest(readyWire).kind == RequestKind::RecorderReady);

    Request cancel = ready;
    cancel.kind = RequestKind::Cancel;
    cancel.sequence = 3;
    cancel.elapsedMilliseconds = 13'000;
    cancel.reason = "HELPER_CANCELLED";
    assert(ParseRequest(EncodeRequest(cancel)).kind == RequestKind::Cancel);
    assert(EncodeRequest(cancel) == std::string(
        "IVRDROID_CALL_CONTROL_V2 CANCEL ") + kSession + " 17 " + kBlock +
        " 3 " + kBoot + " 13000 HELPER_CANCELLED\n");
    cancel.reason = "ANSWER_TIMEOUT";
    assert(EncodeRequest(cancel) == std::string(
        "IVRDROID_CALL_CONTROL_V2 CANCEL ") + kSession + " 17 " + kBlock +
        " 3 " + kBoot + " 13000 ANSWER_TIMEOUT\n");
    const Request answerTimeoutCancel = ParseRequest(EncodeRequest(cancel));
    assert(answerTimeoutCancel.kind == RequestKind::Cancel);
    assert(answerTimeoutCancel.reason == "ANSWER_TIMEOUT");
    cancel.reason = "-";
    assert(EncodeRequest(cancel).empty());
    assert(ParseRequest(std::string(
        "IVRDROID_CALL_CONTROL_V2 CANCEL ") + kSession + " 17 " + kBlock +
        " 3 " + kBoot + " 13000\n").kind == RequestKind::Invalid);

    const char* phases[] = {
        "ACK", "CALLER_HELD", "DIALING", "OPERATOR_ANSWERED", "MERGING",
        "CONFERENCED", "COMPLETED", "NOT_CONNECTED", "SYSTEM_FAILURE",
    };
    for (size_t index = 0; index < sizeof(phases) / sizeof(phases[0]); ++index) {
        const Status status = ParseStatus(StatusWire(phases[index], index + 1));
        assert(status.kind != StatusKind::Invalid);
        assert(status.sequence == index + 1);
        assert(status.elapsedMilliseconds == 42'000);
    }
    assert(ParseStatus(StatusWire("SYSTEM_FAILURE", 10, "RECORDER_FAILED")).reason ==
        "RECORDER_FAILED");

    std::string noNewline = Dial();
    noNewline.pop_back();
    assert(ParseRequest(noNewline).kind == RequestKind::Invalid);
    assert(ParseRequest(Dial() + "\n").kind == RequestKind::Invalid);
    assert(ParseRequest(std::string(513, 'A')).kind == RequestKind::Invalid);

    std::string leadingZeroRevision = Dial();
    leadingZeroRevision.replace(leadingZeroRevision.find(" 17 "), 4, " 017 ");
    assert(ParseRequest(leadingZeroRevision).kind == RequestKind::Invalid);
    std::string zeroSequence = Dial();
    zeroSequence.replace(zeroSequence.find(" 1 " + std::string(kBoot)), 3, " 0 ");
    assert(ParseRequest(zeroSequence).kind == RequestKind::Invalid);
    std::string leadingZeroElapsed = Dial();
    leadingZeroElapsed.replace(leadingZeroElapsed.find(" 12345 "), 7, " 012345 ");
    assert(ParseRequest(leadingZeroElapsed).kind == RequestKind::Invalid);
    std::string shortNumber = Dial();
    shortNumber.replace(shortNumber.find("+982112345678"), 13, "+1234567");
    assert(ParseRequest(shortNumber).kind == RequestKind::Invalid);
    std::string longTimeout = Dial();
    longTimeout.replace(longTimeout.find("30000"), 5, "120001");
    assert(ParseRequest(longTimeout).kind == RequestKind::Invalid);
    std::string fractionalSecondTimeout = Dial();
    fractionalSecondTimeout.replace(
        fractionalSecondTimeout.find("30000"), 5, "30500");
    assert(ParseRequest(fractionalSecondTimeout).kind == RequestKind::Invalid);

    assert(ParseStatus(StatusWire("DIALING", 0)).kind == StatusKind::Invalid);
    assert(ParseStatus(StatusWire("DIALING", 1, "lowercase")).kind == StatusKind::Invalid);
    assert(ParseStatus(StatusWire("UNKNOWN", 1)).kind == StatusKind::Invalid);
    assert(ParseStatus(StatusWire("DIALING", 1) + "junk\n").kind == StatusKind::Invalid);

    assert(IsPhoneNumber("+982112345678"));
    assert(IsPhoneNumber("02112345678"));
    assert(!IsPhoneNumber("+1234567"));
    assert(!IsPhoneNumber("+1234567890123456"));
    assert(IsReason("NO_ANSWER"));
    assert(IsReason("-"));
    assert(!IsReason("NO-ANSWER"));

    assert(argc == 7);
    const Request sharedDial = ParseRequest(ReadFixture(argv[1]));
    const Request sharedReady = ParseRequest(ReadFixture(argv[2]));
    const Request sharedCancel = ParseRequest(ReadFixture(argv[3]));
    const Status sharedStatus = ParseStatus(ReadFixture(argv[4]));
    const Request sharedAnswerTimeoutCancel = ParseRequest(ReadFixture(argv[5]));
    const Status sharedAnswerTimeoutTerminal = ParseStatus(ReadFixture(argv[6]));
    assert(sharedDial.kind == RequestKind::Dial && sharedDial.sequence == 1);
    assert(sharedReady.kind == RequestKind::RecorderReady &&
        sharedReady.sequence == 2);
    assert(sharedCancel.kind == RequestKind::Cancel &&
        sharedCancel.reason == "HELPER_CANCELLED");
    assert(sharedStatus.kind == StatusKind::Merging && sharedStatus.sequence == 7);
    assert(sharedAnswerTimeoutCancel.kind == RequestKind::Cancel &&
        sharedAnswerTimeoutCancel.sequence == 2 &&
        sharedAnswerTimeoutCancel.reason == "ANSWER_TIMEOUT");
    assert(sharedAnswerTimeoutTerminal.kind == StatusKind::NotConnected &&
        sharedAnswerTimeoutTerminal.sequence == 8 &&
        sharedAnswerTimeoutTerminal.reason == "ANSWER_TIMEOUT");
    assert(sharedAnswerTimeoutCancel.sessionUuid ==
        sharedAnswerTimeoutTerminal.sessionUuid);
    assert(sharedAnswerTimeoutCancel.revisionId ==
        sharedAnswerTimeoutTerminal.revisionId);
    assert(sharedAnswerTimeoutCancel.blockUuid ==
        sharedAnswerTimeoutTerminal.blockUuid);
    assert(sharedAnswerTimeoutCancel.bootUuid ==
        sharedAnswerTimeoutTerminal.bootUuid);
    assert(EncodeRequest(sharedDial) == ReadFixture(argv[1]));
    assert(EncodeRequest(sharedReady) == ReadFixture(argv[2]));
    assert(EncodeRequest(sharedCancel) == ReadFixture(argv[3]));
    assert(EncodeRequest(sharedAnswerTimeoutCancel) == ReadFixture(argv[5]));

    std::cout << "Call-control protocol tests passed." << std::endl;
    return 0;
}
