#include "continuous_recording.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main() {
    using namespace ivrdroid;
    ContinuousRecordingContext c;
    c.kind = "conversation"; c.id = "00000000-0000-4000-8000-000000000001";
    c.callId = "00000000-0000-4000-8000-000000000002";
    c.bootId = "00000000-0000-4000-8000-000000000003";
    c.blockId = "00000000-0000-4000-8000-000000000004";
    c.revision = 21; c.wallMs = 1700000000000; c.elapsedMs = 1000; c.pid = getpid(); c.processStart = 1;
    ContinuousRecordingContext parsed;
    assert(ParseContinuousContext(FormatContinuousContext(c), &parsed));
    assert(parsed.wallMs == c.wallMs && parsed.id == c.id);
    assert(!ParseContinuousContext(FormatContinuousContext(c) + "extra", &parsed));
    char pattern[] = "/tmp/ivrdroid-pcm-test.XXXXXX";
    const std::string directory = mkdtemp(pattern);
    std::vector<int16_t> frame(2400, 1234);
    {
        ContinuousPcmFile file(directory, getuid(), c);
        assert(file.Open());
        // Grow beyond both old physical boundaries without creating another audio file.
        for (int i = 0; i < 6 * 60 * 40 + 1; ++i) {
            assert(file.Append(frame.data(), 1200, 100000000));
            if (i % 40 == 39) assert(file.Checkpoint());
        }
        assert(file.frames() == (6 * 60 * 40 + 1) * 1200ULL);
        assert(!file.Append(frame.data(), 1200, file.frames() * 4));
        // Simulate process death after a write and before the next checkpoint.
    }
    std::ifstream in(directory + "/continuous.context");
    const std::string checkpoint((std::istreambuf_iterator<char>(in)), {});
    assert(ParseContinuousContext(checkpoint, &parsed));
    assert(parsed.frames == 6 * 60 * 48000ULL);
    assert(ContinuousPcmFile::Recover(directory, getuid(), parsed));
    struct stat st {};
    assert(stat((directory + "/audio.pcm").c_str(), &st) == 0);
    assert(static_cast<uint64_t>(st.st_size) == parsed.frames * 4);
    std::ifstream seal(directory + "/continuous.sealed"); std::string reason; int partial;
    seal >> reason >> partial; assert(reason == "interrupted" && partial == 1);
    unlink((directory + "/audio.pcm").c_str()); unlink((directory + "/continuous.context").c_str());
    unlink((directory + "/continuous.sealed").c_str()); rmdir(directory.c_str());
    std::cout << "Continuous PCM checkpoint, boundary and recovery tests passed.\n";
}
