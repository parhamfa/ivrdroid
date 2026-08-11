#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ivrdroid::call_control {

constexpr size_t kMaximumWireBytes = 512;
constexpr uint32_t kMinimumAnswerTimeoutMilliseconds = 5'000;
constexpr uint32_t kMaximumAnswerTimeoutMilliseconds = 120'000;
constexpr int64_t kHeartbeatIntervalMilliseconds = 1'000;
constexpr int64_t kHeartbeatMaximumAgeMilliseconds = 3'000;
constexpr int64_t kSetupMaximumMilliseconds = 10'000;
constexpr int64_t kMergeMaximumMilliseconds = 10'000;

enum class RequestKind {
    Dial,
    RecorderReady,
    Cancel,
    Invalid,
};

struct Request {
    RequestKind kind = RequestKind::Invalid;
    std::string sessionUuid;
    uint64_t revisionId = 0;
    std::string blockUuid;
    uint64_t sequence = 0;
    std::string bootUuid;
    uint64_t elapsedMilliseconds = 0;
    std::string phoneNumber;
    uint32_t answerTimeoutMilliseconds = 0;
    std::string reason;
};

enum class StatusKind {
    Ack,
    CallerHeld,
    Dialing,
    OperatorAnswered,
    Merging,
    Conferenced,
    Completed,
    NotConnected,
    SystemFailure,
    Invalid,
};

struct Status {
    StatusKind kind = StatusKind::Invalid;
    std::string sessionUuid;
    uint64_t revisionId = 0;
    std::string blockUuid;
    uint64_t sequence = 0;
    std::string bootUuid;
    uint64_t elapsedMilliseconds = 0;
    std::string reason;
};

bool IsCanonicalUuid(std::string_view value);
bool IsPhoneNumber(std::string_view value);
bool IsReason(std::string_view value);
Request ParseRequest(std::string_view wire);
Status ParseStatus(std::string_view wire);
std::string EncodeRequest(const Request& request);
const char* ToString(RequestKind kind);
const char* ToString(StatusKind kind);

}  // namespace ivrdroid::call_control
