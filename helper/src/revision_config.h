#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ivrdroid {

enum class RevisionNodeType {
    PlayPrompt,
    CollectDigit,
    ScheduleBranch,
    RepeatMenu,
    EndCall,
};

enum class RevisionInstructionType {
    PlayPrompt,
    CollectDigit,
    ScheduleBranch,
    ReturnToMenu,
    RecordMessage,
    ExternalCall,
    EndCall,
};

struct PromptAsset {
    std::string id;
    std::string sha256;
    uint64_t sizeBytes = 0;
};

struct ScheduleWindow {
    std::string scheduleId;
    bool holiday = false;
    int64_t startEpochSeconds = 0;
    int64_t endEpochSeconds = 0;
};

struct RevisionNode {
    std::string id;
    RevisionNodeType type = RevisionNodeType::EndCall;
    std::string promptId;
    std::string next;
    uint32_t timeoutMilliseconds = 0;
    uint32_t maximumAttempts = 0;
    std::unordered_map<char, std::string> digitBranches;
    std::string onTimeout;
    std::string onInvalid;
    std::string scheduleId;
    std::string onOpen;
    std::string onClosed;
    std::string onHoliday;
    std::string repeatTarget;
    uint32_t maximumRepeats = 0;
    std::string onExhausted;
};

struct RevisionInstruction {
    uint32_t pc = 0;
    std::string blockId;
    RevisionInstructionType type = RevisionInstructionType::EndCall;
    std::string promptId;
    uint32_t nextPc = 0;
    uint32_t timeoutMilliseconds = 0;
    uint32_t maximumAttempts = 0;
    uint32_t maximumMenuReturns = 0;
    bool allowPromptBargeIn = false;
    std::unordered_map<char, uint32_t> digitBranches;
    uint32_t onTimeoutPc = 0;
    uint32_t onInvalidPc = 0;
    uint32_t onReturnLimitPc = 0;
    std::string scheduleId;
    uint32_t onOpenPc = 0;
    uint32_t onClosedPc = 0;
    uint32_t onHolidayPc = 0;
    uint32_t menuPc = 0;
    uint32_t maximumDurationMilliseconds = 0;
    char finishKey = 0;
    uint32_t onUnavailablePc = 0;
    std::string externalNumber;
    uint32_t answerTimeoutMilliseconds = 0;
    uint32_t onCompletedPc = 0;
    uint32_t onNotConnectedPc = 0;
    uint32_t onSystemFailurePc = 0;
};

struct RevisionConfig {
    uint32_t schemaVersion = 1;
    uint64_t revisionId = 0;
    std::string manifestSha256;
    std::string sourceSha256;
    std::string programSha256;
    int64_t horizonStartEpochSeconds = 0;
    int64_t horizonEndEpochSeconds = 0;
    std::string rootNode;
    std::vector<std::string> schedules;
    std::vector<PromptAsset> prompts;
    std::vector<ScheduleWindow> windows;
    std::vector<RevisionNode> nodes;
    uint32_t entryPc = 0;
    uint32_t maximumSessionMilliseconds = 0;
    std::vector<RevisionInstruction> instructions;

    const RevisionNode* FindNode(const std::string& id) const;
    const RevisionInstruction* FindInstruction(uint32_t pc) const;
    const PromptAsset* FindPrompt(const std::string& id) const;
    bool IsScheduleOpen(
        const std::string& scheduleId,
        int64_t epochSeconds,
        bool* holiday) const;
};

bool ParseRevisionConfig(
    std::string_view document,
    RevisionConfig* output,
    std::string* error);

}  // namespace ivrdroid
