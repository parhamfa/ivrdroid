#include "conversation_handoff_protocol.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr char kCall[] = "11111111-1111-4111-8111-111111111111";
constexpr char kBlock[] = "22222222-2222-4222-8222-222222222222";
constexpr char kRecording[] = "33333333-3333-4333-8333-333333333333";
constexpr char kBoot[] = "44444444-4444-4444-8444-444444444444";

std::string Ack(
    uint32_t segmentIndex,
    uint64_t elapsedMilliseconds,
    const char* result,
    const char* reason) {
    return std::string("IVRDROID_CONVERSATION_HANDOFF_V1 ") + kCall +
        " 17 " + kBlock + " " + kRecording + " " +
        std::to_string(segmentIndex) + " " + kBoot + " " +
        std::to_string(elapsedMilliseconds) + " " + result + " " + reason +
        "\n";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ivrdroid::conversation_handoff;

    const Acknowledgement parsed = ParseAcknowledgement(Ack(0, 42'000, "OK", "-"));
    assert(parsed.result == Result::Ok);
    assert(parsed.callUuid == kCall);
    assert(parsed.revisionId == 17);
    assert(parsed.blockUuid == kBlock);
    assert(parsed.recordingUuid == kRecording);
    assert(parsed.segmentIndex == 0);
    assert(parsed.bootUuid == kBoot);
    assert(parsed.elapsedMilliseconds == 42'000);
    assert(parsed.reason == "-");
    assert(EncodeAcknowledgement(parsed) == Ack(0, 42'000, "OK", "-"));

    Acknowledgement failed = parsed;
    failed.segmentIndex = 1;
    failed.elapsedMilliseconds = 43'000;
    failed.result = Result::Failed;
    failed.reason = "ENCRYPTION_FAILED";
    assert(ParseAcknowledgement(EncodeAcknowledgement(failed)).result == Result::Failed);

    assert(ParseAcknowledgement(Ack(0, 42'000, "OK", "NOT_DASH")).result ==
        Result::Invalid);
    assert(ParseAcknowledgement(Ack(0, 42'000, "FAILED", "-")).result ==
        Result::Invalid);
    assert(ParseAcknowledgement(Ack(65'536, 42'000, "OK", "-")).result ==
        Result::Invalid);
    assert(ParseAcknowledgement(Ack(0, 42'000, "UNKNOWN", "-")).result ==
        Result::Invalid);
    assert(ParseAcknowledgement(Ack(0, 42'000, "OK", "-") + "\n").result ==
        Result::Invalid);
    assert(ParseAcknowledgement(std::string(513, 'A')).result == Result::Invalid);

    Policy policy(kCall, 17, kBlock, kRecording, kBoot);
    assert(policy.valid());
    assert(policy.MarkSegmentFinalized(0, 1'000));
    assert(policy.pending());
    assert(policy.CheckDeadline(11'000) == Decision::Awaiting);
    assert(policy.Observe(parsed, 11'000) == Decision::Accepted);
    assert(!policy.pending());
    assert(policy.nextExpectedSegmentIndex() == 1);

    assert(policy.Observe(parsed, 11'001) == Decision::Stale);
    assert(policy.MarkSegmentFinalized(1, 12'000));
    Acknowledgement future = failed;
    future.segmentIndex = 2;
    assert(policy.Observe(future, 12'001) == Decision::ProtocolFailure);
    assert(policy.CheckDeadline(22'001) == Decision::TimedOut);

    Policy failurePolicy(kCall, 17, kBlock, kRecording, kBoot);
    assert(failurePolicy.MarkSegmentFinalized(0, 2'000));
    Acknowledgement firstFailure = failed;
    firstFailure.segmentIndex = 0;
    assert(failurePolicy.Observe(firstFailure, 2'001) == Decision::Failed);

    Policy elapsedPolicy(kCall, 17, kBlock, kRecording, kBoot);
    assert(elapsedPolicy.MarkSegmentFinalized(0, 3'000));
    assert(elapsedPolicy.Observe(parsed, 3'001) == Decision::Accepted);
    assert(elapsedPolicy.MarkSegmentFinalized(1, 4'000));
    failed.result = Result::Ok;
    failed.reason = "-";
    failed.elapsedMilliseconds = 41'999;
    assert(elapsedPolicy.Observe(failed, 4'001) == Decision::ProtocolFailure);

    Acknowledgement foreign = parsed;
    foreign.recordingUuid = "55555555-5555-4555-8555-555555555555";
    Policy foreignPolicy(kCall, 17, kBlock, kRecording, kBoot);
    assert(foreignPolicy.MarkSegmentFinalized(0, 5'000));
    assert(foreignPolicy.Observe(foreign, 5'001) == Decision::IgnoredForeign);

    assert(argc == 2);
    std::ifstream fixture(argv[1]);
    std::ostringstream document;
    document << fixture.rdbuf();
    assert(fixture.good() || fixture.eof());
    const Acknowledgement shared = ParseAcknowledgement(document.str());
    assert(shared.result == Result::Ok && shared.segmentIndex == 0);
    assert(EncodeAcknowledgement(shared) == document.str());

    std::cout << "Conversation handoff protocol tests passed." << std::endl;
    return 0;
}
