#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <tinyalsa/asoundlib.h>

namespace ivrdroid {

// All consumers receive original carrier PCM. Only the independent audit writer mixes prompts.
struct SessionCapture;
void RefreshAuditPolicy(uid_t appUid);
void RecoverAuditRecordings(uid_t appUid);
void PrepareSessionAudio(const std::string& callUuid, uid_t appUid, int card, int device);
void SpawnSessionAudioWorkers(); // called only in the guardian, before its monitor starts
void ActivateSessionAudio();
void StopSessionAudio(const char* reason);
void DrainSessionAudioWorkers();
void SuperviseSessionAudioWorkers();
void ReleaseSessionAudio();
struct ConversationWriter;
ConversationWriter* StartContinuousConversation(const std::string& recordingId, const std::string& callId,
    uint64_t revisionId, const std::string& blockId);
bool ContinuousConversationHealthy(ConversationWriter* writer);
void StopContinuousConversation(ConversationWriter* writer, const char* reason, bool partial);
void AuditEvent(const char* type, const std::string& block = "", const std::string& detail = "");
void BeginAuditPrompt();
void TapAuditPrompt(pcm* output, const int16_t* samples, unsigned int frames);
void EndAuditPrompt();

SessionCapture* OpenSessionCapture(unsigned int card, unsigned int device, unsigned int flags, const pcm_config* config);
bool SessionCaptureReady(SessionCapture* input);
int ReadSessionCapture(SessionCapture* input, void* samples, unsigned int frames);
int StartSessionCapture(SessionCapture* input);
const char* SessionCaptureError(SessionCapture* input);
void CloseSessionCapture(SessionCapture* input);

} // namespace ivrdroid
