// Executes the production IPC, writer, checkpoint and recovery implementation against a fake
// carrier PCM. No Telecom, mixer, microphone or production bridge is accessed by this binary.
#include "../src/session_audio.cpp"
#include <cassert>
#include <filesystem>
#include <iostream>

struct pcm { bool capture = false; };
struct FakeState { std::atomic<int> opens {0}, active {0}, failAfter {0}, reads {0}, queued {0}; };
FakeState* fake = nullptr;

extern "C" {
pcm* pcm_open(unsigned int, unsigned int, unsigned int flags, const pcm_config*) {
    auto* value = new pcm {bool(flags & PCM_IN)};
    if (value->capture) {
        assert(fake->active.fetch_add(1) == 0);
        fake->opens.fetch_add(1);
    }
    return value;
}
int pcm_is_ready(const pcm* value) { return value != nullptr; }
int pcm_close(pcm* value) { if (value->capture) fake->active.fetch_sub(1); delete value; return 0; }
int pcm_start(pcm*) { return 0; }
const char* pcm_get_error(const pcm*) { return "Synthetic capture"; }
unsigned int pcm_get_buffer_size(const pcm*) { return fake->queued.load(); }
int pcm_get_htimestamp(pcm*, unsigned int* available, timespec* stamp) {
    *available = 0; return clock_gettime(CLOCK_MONOTONIC, stamp);
}
int pcm_readi(pcm*, void* data, unsigned int frames) {
    usleep(frames * 1'000'000ULL / 48000);
    const int reads = fake->reads.fetch_add(1) + 1;
    if (fake->failAfter.load() > 0 && reads >= fake->failAfter.load()) return -1;
    std::fill_n(static_cast<int16_t*>(data), frames * 2, 1000);
    return frames;
}
}

namespace {
const std::string id = "23d4b16c-f15b-4de4-9444-d08265e194ab";
const std::string bridge = IVRDROID_AUDIT_BRIDGE;
const uid_t testUid = geteuid() == 0 ? 2000 : getuid();
std::string folder() { return bridge + "/session-audit/" + id; }
void start(bool auditing = true) {
    ivrdroid::ReleaseSessionAudio();
    std::filesystem::remove_all(bridge);
    std::filesystem::create_directories(bridge);
    fake->opens = 0; fake->active = 0; fake->failAfter = 0; fake->reads = 0; fake->queued = 0;
    ivrdroid::appUid = testUid;
    assert(ivrdroid::AtomicFile(bridge + "/audit-policy", std::string("version=1\nenabled=") + (auditing ? "1" : "0") + "\nquota=1073741824\n"));
    assert(ivrdroid::AtomicFile(bridge + "/audit-capacity", "0\n"));
    assert(ivrdroid::AtomicFile(bridge + "/continuous-capacity", "PCM1 0 0 0\n"));
    ivrdroid::RefreshAuditPolicy(testUid);
    ivrdroid::PrepareSessionAudio(id, testUid, 0, 0);
    ivrdroid::SpawnSessionAudioWorkers();
    ivrdroid::ActivateSessionAudio();
    assert(ivrdroid::shared && ivrdroid::shared->ready.load());
}
void waitFrames(uint64_t frames) {
    const int64_t deadline = ivrdroid::ClockNs(CLOCK_MONOTONIC) + 400'000'000'000LL;
    while (ivrdroid::shared->produced.load() < frames) {
        assert(ivrdroid::ClockNs(CLOCK_MONOTONIC) < deadline); usleep(2000);
    }
}
std::string finish(const char* reason) {
    ivrdroid::StopSessionAudio(reason);
    ivrdroid::DrainSessionAudioWorkers();
    ivrdroid::ReleaseSessionAudio();
    ivrdroid::RecoverAuditRecordings(testUid);
    return ivrdroid::ReadFile(folder() + "/report.json", 1024 * 1024);
}
int16_t sample(const std::string& path, size_t frame) {
    std::ifstream input(path, std::ios::binary); input.seekg(frame * 4);
    int16_t value = 0; input.read(reinterpret_cast<char*>(&value), sizeof(value)); assert(input.good()); return value;
}
}

int main(int argc, char** argv) {
    const bool longRun = argc == 2 && std::string(argv[1]) == "--long";
    void* state = mmap(nullptr, sizeof(FakeState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(state != MAP_FAILED); fake = new (state) FakeState();
    start();
    const std::string conversationId = "923d4b16-cf15-4de4-9444-d08265e194ab";
    const std::string blockId = "22222222-2222-4222-8222-222222222222";
    const std::string conversationFolder = bridge + "/continuous-recordings/" + conversationId;
    auto* conversation = ivrdroid::StartContinuousConversation(conversationId, id, 21, blockId);
    assert(conversation);
    pcm_config config {}; config.channels = 2; config.rate = 48000;
    auto* consumer = ivrdroid::OpenSessionCapture(0, 0, PCM_IN, &config);
    assert(ivrdroid::SessionCaptureReady(consumer));
    int16_t carrier[2400] {};
    assert(ivrdroid::ReadSessionCapture(consumer, carrier, 1200) == 1200 && carrier[0] == 1000);
    ivrdroid::CloseSessionCapture(consumer);
    consumer = ivrdroid::OpenSessionCapture(0, 0, PCM_IN, &config);
    assert(fake->opens.load() == 1 && fake->active.load() == 1);
    waitFrames(12);
    // Queue 100ms, then interrupt after 20ms. The audit must include rendered samples only.
    pcm output {};
    std::vector<int16_t> prompt(4800 * 2, 500);
    fake->queued = 4800;
    ivrdroid::BeginAuditPrompt();
    const int64_t promptAt = ivrdroid::ClockNs(CLOCK_MONOTONIC);
    ivrdroid::TapAuditPrompt(&output, prompt.data(), 4800);
    usleep(20'000); ivrdroid::EndAuditPrompt();
    const int64_t promptEnd = ivrdroid::ClockNs(CLOCK_MONOTONIC);
    const int64_t base = ivrdroid::shared->baseNs.load();
    assert(ivrdroid::ReadSessionCapture(consumer, carrier, 1200) == 1200 && carrier[0] == 1000);
    ivrdroid::CloseSessionCapture(consumer);
    waitFrames(longRun ? 14500 : 650);
    assert(ivrdroid::ContinuousConversationHealthy(conversation));
    const pid_t conversationPid = conversation->pid;
    ivrdroid::StopContinuousConversation(conversation, "caller_hangup", false);
    int conversationStatus = 0;
    assert(waitpid(conversationPid, &conversationStatus, 0) == conversationPid && WIFEXITED(conversationStatus) && WEXITSTATUS(conversationStatus) == 0);
    auto report = finish("caller_hangup");
    assert(report.find("\"partial\":false") != std::string::npos);
    assert(std::filesystem::exists(folder() + "/continuous.sealed"));
    assert(!std::filesystem::exists(folder() + "/00000.wav"));
    assert(sample(folder() + "/audio.pcm", (promptAt - base + 5'000'000) * 48000 / 1'000'000'000LL) == 1500);
    assert(sample(folder() + "/audio.pcm", (promptEnd - base + 30'000'000) * 48000 / 1'000'000'000LL) == 1000);
    assert(sample(conversationFolder + "/audio.pcm", 4800) == 1000);
    if (longRun) assert(std::filesystem::file_size(conversationFolder + "/audio.pcm") > 360ULL * 192000);
    std::cout << "PASS: both continuous writers, independent PCM, rendered prompt interruption, no timed rotation" << std::endl;

    start(); waitFrames(95); // 2.375s: only completed one-second checkpoints are recoverable.
    kill(ivrdroid::writerPid, SIGKILL); waitpid(ivrdroid::writerPid, nullptr, 0); ivrdroid::writerPid = -1;
    report = finish("interrupted");
    assert(report.find("\"duration_ms\":2000") != std::string::npos);
    assert(report.find("\"partial\":true") != std::string::npos);
    assert(std::filesystem::file_size(folder() + "/audio.pcm") == 2000 * 192);
    std::cout << "PASS: interrupted writer recovers only the last durable checkpoint\n";

    start(); waitFrames(40);
    fake->failAfter = fake->reads.load() + 1;
    while (!ivrdroid::shared->failed.load()) usleep(2000);
    usleep(60'000); // The guardian classifies failure after the last captured frame.
    report = finish("capture_failure");
    assert(report.find("\"partial\":true") != std::string::npos);
    assert(report.find("\"type\":\"gap\"") < report.find("\"type\":\"ended\""));
    assert(report.find("\"type\":\"ended\"") != std::string::npos);
    assert(report.find("\"stop_reason\":\"capture_failure\"") != std::string::npos);
    std::cout << "PASS: capture failure keeps the coverage boundary before the later disconnect\n";

    start(); waitFrames(20);
    assert(ivrdroid::AtomicFile(folder() + "/abort", "writer_failure\n"));
    waitFrames(80);
    consumer = ivrdroid::OpenSessionCapture(0, 0, PCM_IN, &config);
    assert(ivrdroid::ReadSessionCapture(consumer, carrier, 1200) == 1200 && carrier[0] == 1000);
    ivrdroid::CloseSessionCapture(consumer);
    assert(fake->opens.load() == 1);
    report = finish("caller_hangup");
    assert(report.find("writer_failure") != std::string::npos && report.find("\"partial\":true") != std::string::npos);
    std::cout << "PASS: audit handoff failure preserves call capture and marks partial coverage\n";

    start();
    assert(ivrdroid::AtomicFile(bridge + "/continuous-capacity", "PCM1 0 1073741824 0\n"));
    waitFrames(40);
    report = finish("caller_hangup");
    assert(report.find("storage_full") != std::string::npos);
    std::cout << "PASS: audit quota preserves recording headroom\n";

    start(); waitFrames(50);
    kill(ivrdroid::writerPid, SIGSTOP);
    waitFrames(210);
    consumer = ivrdroid::OpenSessionCapture(0, 0, PCM_IN, &config);
    assert(ivrdroid::ReadSessionCapture(consumer, carrier, 1200) == 1200 && carrier[0] == 1000);
    ivrdroid::CloseSessionCapture(consumer);
    kill(ivrdroid::writerPid, SIGCONT);
    waitFrames(230);
    report = finish("caller_hangup");
    assert(report.find("buffer_overrun") != std::string::npos && report.find("\"partial\":true") != std::string::npos);
    std::cout << "PASS: blocked audit writer leaves capture running and retains a partial prefix\n";

    start(false);
    conversation = ivrdroid::StartContinuousConversation(conversationId, id, 21, blockId);
    assert(conversation && ivrdroid::writerPid <= 0);
    waitFrames(100);
    const pid_t independentPid = conversation->pid;
    ivrdroid::StopContinuousConversation(conversation, "operator_hangup", false);
    assert(waitpid(independentPid, &conversationStatus, 0) == independentPid && WIFEXITED(conversationStatus) && WEXITSTATUS(conversationStatus) == 0);
    finish("caller_hangup");
    assert(std::filesystem::file_size(conversationFolder + "/audio.pcm") > 192000);
    assert(!std::filesystem::exists(folder() + "/audio.pcm"));
    std::cout << "PASS: conversation recording remains available with whole-call auditing disabled\n";
    std::filesystem::remove_all(bridge);
    munmap(state, sizeof(FakeState));
}
