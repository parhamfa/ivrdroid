#include "revision_config.h"

#include "call_control_protocol.h"
#include "sha256.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace ivrdroid {
namespace {

constexpr size_t kMaximumConfigBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumNodes = 64;
constexpr size_t kMaximumPrompts = 64;
constexpr size_t kMaximumSchedules = 32;
constexpr size_t kMaximumWindows = 65'536;
constexpr uint64_t kMaximumRevisionAssetBytes = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumPromptBytes = 64ULL * 1024ULL * 1024ULL;

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

bool IsIdentifier(std::string_view value) {
    if (value.empty() || value.size() > 32 ||
        value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool IsPromptIdentifier(std::string_view value) {
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen ? character != '-' :
            !((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsBlockIdentifier(std::string_view value) {
    if (!IsPromptIdentifier(value)) return false;
    const char version = value[14];
    const char variant = value[19];
    return version >= '1' && version <= '5' &&
        (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

template <typename Integer>
bool ParseInteger(const std::string& value, Integer* output) {
    if (value.empty()) return false;
    Integer parsed {};
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    *output = parsed;
    return true;
}

std::vector<std::string> Split(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> result;
    std::string token;
    while (stream >> token) result.push_back(token);
    return result;
}

std::vector<std::string> RegularEdges(const RevisionNode& node) {
    switch (node.type) {
        case RevisionNodeType::PlayPrompt:
            return {node.next};
        case RevisionNodeType::CollectDigit: {
            std::vector<std::string> edges {node.onTimeout, node.onInvalid};
            for (const auto& branch : node.digitBranches) edges.push_back(branch.second);
            return edges;
        }
        case RevisionNodeType::ScheduleBranch:
            return {node.onOpen, node.onClosed, node.onHoliday};
        case RevisionNodeType::RepeatMenu:
            return {node.onExhausted};
        case RevisionNodeType::EndCall:
            return {};
    }
    return {};
}

std::vector<std::string> AllEdges(const RevisionNode& node) {
    std::vector<std::string> edges = RegularEdges(node);
    if (node.type == RevisionNodeType::RepeatMenu) {
        edges.push_back(node.repeatTarget);
    }
    return edges;
}

bool ValidateGraph(const RevisionConfig& config, std::string* error) {
    std::unordered_map<std::string, const RevisionNode*> nodes;
    for (const RevisionNode& node : config.nodes) nodes.emplace(node.id, &node);
    if (nodes.find(config.rootNode) == nodes.end()) {
        return Fail(error, "root node is missing");
    }
    std::unordered_set<std::string> reachable;
    std::vector<std::string> pending {config.rootNode};
    while (!pending.empty()) {
        const std::string id = pending.back();
        pending.pop_back();
        if (!reachable.insert(id).second) continue;
        const auto found = nodes.find(id);
        if (found == nodes.end()) return Fail(error, "node target is missing");
        for (const std::string& target : AllEdges(*found->second)) pending.push_back(target);
    }
    if (reachable.size() != nodes.size()) return Fail(error, "flow has unreachable nodes");

    std::unordered_set<std::string> complete;
    std::unordered_set<std::string> active;
    std::function<bool(const std::string&, unsigned int)> visit =
        [&](const std::string& id, unsigned int depth) -> bool {
            if (depth > 8) return false;
            if (complete.count(id) != 0) return true;
            if (!active.insert(id).second) return false;
            const RevisionNode& node = *nodes.at(id);
            for (const std::string& target : RegularEdges(node)) {
                if (!visit(target, depth + 1)) return false;
            }
            active.erase(id);
            complete.insert(id);
            return true;
        };
    if (!visit(config.rootNode, 1)) {
        return Fail(error, "flow has an unbounded cycle or exceeds depth 8");
    }

    std::unordered_map<std::string, bool> terminating;
    std::function<bool(const std::string&, std::unordered_set<std::string>)> canEnd =
        [&](const std::string& id, std::unordered_set<std::string> trail) -> bool {
            const auto cached = terminating.find(id);
            if (cached != terminating.end()) return cached->second;
            if (!trail.insert(id).second) return false;
            const RevisionNode& node = *nodes.at(id);
            if (node.type == RevisionNodeType::EndCall) return terminating[id] = true;
            for (const std::string& target : RegularEdges(node)) {
                if (canEnd(target, trail)) return terminating[id] = true;
            }
            terminating[id] = false;
            return false;
        };
    for (const std::string& id : reachable) {
        if (!canEnd(id, {})) return Fail(error, "flow has a node without a terminating path");
    }
    return true;
}

struct OwnedInstructionEdge {
    uint32_t target = 0;
    bool returnLimit = false;
};

bool ValidateProgramV2(const RevisionConfig& config, std::string* error) {
    if (config.instructions.empty() || config.instructions.size() > kMaximumNodes ||
        config.entryPc >= config.instructions.size()) {
        return Fail(error, "V2 program entry or size is invalid");
    }
    std::vector<uint32_t> incoming(config.instructions.size(), 0);
    std::vector<int64_t> incomingParent(config.instructions.size(), -1);
    std::vector<std::vector<OwnedInstructionEdge>> edges(config.instructions.size());
    auto addEdge = [&](uint32_t source, uint32_t target, bool returnLimit = false) -> bool {
        if (target >= config.instructions.size()) return false;
        ++incoming[target];
        if (incomingParent[target] < 0) {
            incomingParent[target] = static_cast<int64_t>(source);
        }
        edges[source].push_back({target, returnLimit});
        return true;
    };

    for (const RevisionInstruction& instruction : config.instructions) {
        if (instruction.pc >= config.instructions.size() ||
            instruction.pc != &instruction - config.instructions.data()) {
            return Fail(error, "V2 instruction PCs are not contiguous");
        }
        switch (instruction.type) {
            case RevisionInstructionType::PlayPrompt:
                if (!addEdge(instruction.pc, instruction.nextPc)) {
                    return Fail(error, "V2 PLAY target is outside the program");
                }
                break;
            case RevisionInstructionType::CollectDigit:
                for (const auto& branch : instruction.digitBranches) {
                    if (!addEdge(instruction.pc, branch.second)) {
                        return Fail(error, "V2 COLLECT branch is outside the program");
                    }
                }
                if (!addEdge(instruction.pc, instruction.onTimeoutPc) ||
                    !addEdge(instruction.pc, instruction.onInvalidPc) ||
                    !addEdge(instruction.pc, instruction.onReturnLimitPc, true)) {
                    return Fail(error, "V2 COLLECT recovery target is outside the program");
                }
                break;
            case RevisionInstructionType::ScheduleBranch:
                if (!addEdge(instruction.pc, instruction.onOpenPc) ||
                    !addEdge(instruction.pc, instruction.onClosedPc) ||
                    !addEdge(instruction.pc, instruction.onHolidayPc)) {
                    return Fail(error, "V2 SCHEDULE target is outside the program");
                }
                break;
            case RevisionInstructionType::ReturnToMenu:
                if (instruction.menuPc >= config.instructions.size() ||
                    config.instructions[instruction.menuPc].type != RevisionInstructionType::CollectDigit) {
                    return Fail(error, "V2 RETURN target is not a menu");
                }
                break;
            case RevisionInstructionType::RecordMessage:
                if (config.schemaVersion < 3 ||
                    !addEdge(instruction.pc, instruction.nextPc) ||
                    !addEdge(instruction.pc, instruction.onUnavailablePc)) {
                    return Fail(error, "RECORD target is outside the program");
                }
                break;
            case RevisionInstructionType::ExternalCall:
                if (config.schemaVersion != 4 ||
                    !addEdge(instruction.pc, instruction.onCompletedPc) ||
                    !addEdge(instruction.pc, instruction.onNotConnectedPc) ||
                    !addEdge(instruction.pc, instruction.onSystemFailurePc)) {
                    return Fail(error, "V4 EXTERNAL_CALL target is outside the program");
                }
                break;
            case RevisionInstructionType::EndCall:
                break;
        }
    }
    if (incoming[config.entryPc] != 0) {
        return Fail(error, "V2 entry instruction is owned by another instruction");
    }
    for (uint32_t pc = 0; pc < incoming.size(); ++pc) {
        if (pc != config.entryPc && incoming[pc] != 1) {
            return Fail(error, "V2 instruction ownership is not a tree");
        }
        if (config.schemaVersion == 3 &&
            config.instructions[pc].type == RevisionInstructionType::RecordMessage &&
            (incomingParent[pc] < 0 ||
             config.instructions[static_cast<size_t>(incomingParent[pc])].type !=
                RevisionInstructionType::PlayPrompt)) {
            return Fail(error, "RECORD must immediately follow PLAY");
        }
    }

    std::unordered_set<uint32_t> visited;
    std::function<bool(uint32_t, uint32_t, int64_t, int64_t)> visit =
        [&](uint32_t pc, uint32_t depth, int64_t ownerMenu, int64_t forbiddenReturnMenu) -> bool {
            if (depth > 8 || !visited.insert(pc).second) return false;
            const RevisionInstruction& instruction = config.instructions[pc];
            if (instruction.type == RevisionInstructionType::ReturnToMenu) {
                return ownerMenu >= 0 &&
                    instruction.menuPc == static_cast<uint32_t>(ownerMenu) &&
                    static_cast<int64_t>(instruction.menuPc) != forbiddenReturnMenu;
            }
            if (instruction.type == RevisionInstructionType::EndCall) return true;
            for (const OwnedInstructionEdge& edge : edges[pc]) {
                const int64_t nextOwner =
                    instruction.type == RevisionInstructionType::CollectDigit
                    ? static_cast<int64_t>(pc)
                    : ownerMenu;
                const int64_t nextForbidden =
                    instruction.type == RevisionInstructionType::CollectDigit
                    ? (edge.returnLimit ? static_cast<int64_t>(pc) : -1)
                    : forbiddenReturnMenu;
                if (!visit(edge.target, depth + 1, nextOwner, nextForbidden)) return false;
            }
            return true;
        };
    if (!visit(config.entryPc, 1, -1, -1) || visited.size() != config.instructions.size()) {
        return Fail(error, "V2 program is unreachable, cyclic, too deep, or has an invalid menu return");
    }
    if (config.schemaVersion == 4) {
        std::function<bool(uint32_t, bool)> validatePromptNotice =
            [&](uint32_t pc, bool promptNoticeAvailable) -> bool {
                const RevisionInstruction& instruction = config.instructions[pc];
                if (!promptNoticeAvailable) {
                    if (instruction.type == RevisionInstructionType::RecordMessage) {
                        return Fail(
                            error,
                            "Record message requires an earlier Play prompt or menu prompt notice on this path.");
                    }
                    if (instruction.type == RevisionInstructionType::ExternalCall) {
                        return Fail(
                            error,
                            "External call requires an earlier Play prompt or menu prompt notice on this path.");
                    }
                }
                const bool nextPromptNoticeAvailable =
                    promptNoticeAvailable ||
                    instruction.type == RevisionInstructionType::PlayPrompt ||
                    (instruction.type == RevisionInstructionType::CollectDigit &&
                     !instruction.promptId.empty());
                for (const OwnedInstructionEdge& edge : edges[pc]) {
                    if (!validatePromptNotice(edge.target, nextPromptNoticeAvailable)) return false;
                }
                return true;
            };
        if (!validatePromptNotice(config.entryPc, false)) return false;
    }
    return true;
}

}  // namespace

const RevisionNode* RevisionConfig::FindNode(const std::string& id) const {
    for (const RevisionNode& node : nodes) if (node.id == id) return &node;
    return nullptr;
}

const RevisionInstruction* RevisionConfig::FindInstruction(uint32_t pc) const {
    if (pc >= instructions.size() || instructions[pc].pc != pc) return nullptr;
    return &instructions[pc];
}

const PromptAsset* RevisionConfig::FindPrompt(const std::string& id) const {
    for (const PromptAsset& prompt : prompts) if (prompt.id == id) return &prompt;
    return nullptr;
}

bool RevisionConfig::IsScheduleOpen(
    const std::string& scheduleId,
    int64_t epochSeconds,
    bool* holiday) const {
    if (holiday != nullptr) *holiday = false;
    if (epochSeconds < horizonStartEpochSeconds ||
        epochSeconds >= horizonEndEpochSeconds) {
        return false;
    }
    for (const ScheduleWindow& window : windows) {
        if (window.scheduleId == scheduleId &&
            epochSeconds >= window.startEpochSeconds &&
            epochSeconds < window.endEpochSeconds) {
            if (holiday != nullptr) *holiday = window.holiday;
            return !window.holiday;
        }
    }
    return false;
}

bool ParseRevisionConfig(
    std::string_view document,
    RevisionConfig* output,
    std::string* error) {
    if (output == nullptr || document.empty() || document.size() > kMaximumConfigBytes) {
        return Fail(error, "configuration size is invalid");
    }
    RevisionConfig config;
    std::istringstream stream{std::string(document)};
    std::string line;
    bool header = false;
    bool ended = false;
    bool rootSeen = false;
    bool sourceSeen = false;
    bool programSeen = false;
    bool entrySeen = false;
    bool revisionSeen = false;
    bool manifestSeen = false;
    bool horizonSeen = false;
    bool maximumSessionSeen = false;
    std::unordered_set<std::string> nodeIds;
    std::unordered_set<std::string> blockIds;
    std::unordered_set<std::string> promptIds;
    std::unordered_set<std::string> scheduleIds;
    uint64_t assetBytes = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> tokens = Split(line);
        if (tokens.empty()) continue;
        if (!header) {
            if (tokens.size() != 1 ||
                (tokens[0] != "IVRDROID_CONFIG_V1" &&
                 tokens[0] != "IVRDROID_CONFIG_V2" &&
                 tokens[0] != "IVRDROID_CONFIG_V3" &&
                 tokens[0] != "IVRDROID_CONFIG_V4")) {
                return Fail(error, "configuration header is invalid");
            }
            config.schemaVersion = tokens[0] == "IVRDROID_CONFIG_V4"
                ? 4
                : (tokens[0] == "IVRDROID_CONFIG_V3"
                    ? 3
                    : (tokens[0] == "IVRDROID_CONFIG_V2" ? 2 : 1));
            header = true;
            continue;
        }
        if (ended) return Fail(error, "data follows END_CONFIG");
        if (tokens[0] == "END_CONFIG") {
            if (tokens.size() != 1) return Fail(error, "END_CONFIG is invalid");
            ended = true;
        } else if (tokens[0] == "REVISION") {
            if (tokens.size() != 2 || revisionSeen ||
                !ParseInteger(tokens[1], &config.revisionId) || config.revisionId == 0) {
                return Fail(error, "revision is invalid");
            }
            revisionSeen = true;
        } else if (tokens[0] == "MANIFEST") {
            if (tokens.size() != 2 || manifestSeen || !IsLowerHexSha256(tokens[1])) {
                return Fail(error, "manifest hash is invalid");
            }
            config.manifestSha256 = tokens[1];
            manifestSeen = true;
        } else if (tokens[0] == "HORIZON") {
            if (tokens.size() != 3 || horizonSeen ||
                !ParseInteger(tokens[1], &config.horizonStartEpochSeconds) ||
                !ParseInteger(tokens[2], &config.horizonEndEpochSeconds) ||
                config.horizonStartEpochSeconds <= 0 ||
                config.horizonEndEpochSeconds <= config.horizonStartEpochSeconds ||
                config.horizonEndEpochSeconds - config.horizonStartEpochSeconds > 370LL * 86400LL) {
                return Fail(error, "schedule horizon is invalid");
            }
            horizonSeen = true;
        } else if (tokens[0] == "SOURCE") {
            if (config.schemaVersion < 2 || tokens.size() != 2 || sourceSeen ||
                !IsLowerHexSha256(tokens[1])) {
                return Fail(error, "source hash is invalid");
            }
            config.sourceSha256 = tokens[1];
            sourceSeen = true;
        } else if (tokens[0] == "PROGRAM") {
            if (config.schemaVersion < 2 || tokens.size() != 2 || programSeen ||
                !IsLowerHexSha256(tokens[1])) {
                return Fail(error, "program hash is invalid");
            }
            config.programSha256 = tokens[1];
            programSeen = true;
        } else if (tokens[0] == "ENTRY") {
            if (config.schemaVersion < 2 || tokens.size() != 2 || entrySeen ||
                !ParseInteger(tokens[1], &config.entryPc)) {
                return Fail(error, "program entry is invalid");
            }
            entrySeen = true;
        } else if (tokens[0] == "MAX_SESSION") {
            if (config.schemaVersion != 3 || tokens.size() != 2 || maximumSessionSeen ||
                !ParseInteger(tokens[1], &config.maximumSessionMilliseconds) ||
                config.maximumSessionMilliseconds > 600'000) {
                return Fail(error, "V3 maximum session duration is invalid");
            }
            maximumSessionSeen = true;
        } else if (tokens[0] == "MAX_AUTOMATED_SESSION_MS") {
            if (config.schemaVersion != 4 || tokens.size() != 2 || maximumSessionSeen ||
                !ParseInteger(tokens[1], &config.maximumSessionMilliseconds) ||
                config.maximumSessionMilliseconds > 600'000) {
                return Fail(error, "V4 maximum automated session duration is invalid");
            }
            maximumSessionSeen = true;
        } else if (tokens[0] == "ROOT") {
            if (config.schemaVersion != 1 || tokens.size() != 2 || rootSeen ||
                !IsIdentifier(tokens[1])) {
                return Fail(error, "root is invalid");
            }
            config.rootNode = tokens[1];
            rootSeen = true;
        } else if (tokens[0] == "SCHEDULE") {
            if (tokens.size() != 2 || config.schedules.size() >= kMaximumSchedules ||
                !IsIdentifier(tokens[1]) || !scheduleIds.insert(tokens[1]).second) {
                return Fail(error, "schedule is invalid or duplicated");
            }
            config.schedules.push_back(tokens[1]);
        } else if (tokens[0] == "PROMPT") {
            PromptAsset prompt;
            if (tokens.size() != 4 || config.prompts.size() >= kMaximumPrompts ||
                !IsPromptIdentifier(tokens[1]) || !IsLowerHexSha256(tokens[2]) ||
                !ParseInteger(tokens[3], &prompt.sizeBytes) || prompt.sizeBytes <= 44 ||
                prompt.sizeBytes > kMaximumPromptBytes || !promptIds.insert(tokens[1]).second) {
                return Fail(error, "prompt is invalid or duplicated");
            }
            prompt.id = tokens[1];
            prompt.sha256 = tokens[2];
            assetBytes += prompt.sizeBytes;
            if (assetBytes > kMaximumRevisionAssetBytes) {
                return Fail(error, "revision assets exceed 128 MiB");
            }
            config.prompts.push_back(std::move(prompt));
        } else if (tokens[0] == "WINDOW") {
            ScheduleWindow window;
            if (tokens.size() != 5 || config.windows.size() >= kMaximumWindows ||
                !IsIdentifier(tokens[1]) || (tokens[2] != "O" && tokens[2] != "H") ||
                !ParseInteger(tokens[3], &window.startEpochSeconds) ||
                !ParseInteger(tokens[4], &window.endEpochSeconds) ||
                window.startEpochSeconds >= window.endEpochSeconds) {
                return Fail(error, "schedule window is invalid");
            }
            window.scheduleId = tokens[1];
            window.holiday = tokens[2] == "H";
            config.windows.push_back(std::move(window));
        } else if (tokens[0] == "INSTRUCTION") {
            RevisionInstruction instruction;
            if (config.schemaVersion < 2 || tokens.size() < 4 ||
                config.instructions.size() >= kMaximumNodes ||
                !ParseInteger(tokens[1], &instruction.pc) ||
                instruction.pc != config.instructions.size() ||
                !IsBlockIdentifier(tokens[2]) || !blockIds.insert(tokens[2]).second) {
                return Fail(error, "V2 instruction is invalid or duplicated");
            }
            instruction.blockId = tokens[2];
            const std::string& type = tokens[3];
            if (type == "PLAY") {
                if (tokens.size() != 6 || !IsPromptIdentifier(tokens[4]) ||
                    !ParseInteger(tokens[5], &instruction.nextPc)) {
                    return Fail(error, "V2 PLAY instruction is invalid");
                }
                instruction.type = RevisionInstructionType::PlayPrompt;
                instruction.promptId = tokens[4];
            } else if (type == "COLLECT") {
                uint32_t branchCount = 0;
                if (tokens.size() < 12 ||
                    (tokens[4] != "-" && !IsPromptIdentifier(tokens[4])) ||
                    !ParseInteger(tokens[5], &instruction.timeoutMilliseconds) ||
                    instruction.timeoutMilliseconds < 1000 || instruction.timeoutMilliseconds > 15000 ||
                    !ParseInteger(tokens[6], &instruction.maximumAttempts) ||
                    instruction.maximumAttempts < 1 || instruction.maximumAttempts > 3 ||
                    !ParseInteger(tokens[7], &instruction.maximumMenuReturns) ||
                    instruction.maximumMenuReturns < 1 || instruction.maximumMenuReturns > 3 ||
                    !ParseInteger(tokens[8], &instruction.onTimeoutPc) ||
                    !ParseInteger(tokens[9], &instruction.onInvalidPc) ||
                    !ParseInteger(tokens[10], &instruction.onReturnLimitPc) ||
                    !ParseInteger(tokens[11], &branchCount) || branchCount < 1 || branchCount > 12 ||
                    (tokens.size() != 12 + static_cast<size_t>(branchCount) * 2 &&
                     tokens.size() != 13 + static_cast<size_t>(branchCount) * 2)) {
                    return Fail(error, "V2 COLLECT instruction is invalid");
                }
                instruction.type = RevisionInstructionType::CollectDigit;
                instruction.promptId = tokens[4] == "-" ? "" : tokens[4];
                const size_t canonicalSize =
                    12 + static_cast<size_t>(branchCount) * 2;
                instruction.allowPromptBargeIn = tokens.size() == canonicalSize + 1;
                if (instruction.allowPromptBargeIn &&
                    (config.schemaVersion != 4 || tokens.back() != "BARGE_IN" ||
                     instruction.promptId.empty())) {
                    return Fail(error, "V4 COLLECT prompt interruption marker is invalid");
                }
                size_t previousRank = 0;
                bool hasPreviousRank = false;
                for (uint32_t branch = 0; branch < branchCount; ++branch) {
                    const std::string& digit = tokens[12 + branch * 2];
                    uint32_t target = 0;
                    const size_t rank = digit.size() == 1
                        ? std::string("0123456789*#").find(digit[0])
                        : std::string::npos;
                    if (rank == std::string::npos || (hasPreviousRank && rank <= previousRank) ||
                        !ParseInteger(tokens[13 + branch * 2], &target) ||
                        !instruction.digitBranches.emplace(digit[0], target).second) {
                        return Fail(error, "V2 COLLECT branch is invalid, duplicated, or unordered");
                    }
                    previousRank = rank;
                    hasPreviousRank = true;
                }
            } else if (type == "SCHEDULE") {
                if (tokens.size() != 8 || !IsIdentifier(tokens[4]) ||
                    !ParseInteger(tokens[5], &instruction.onOpenPc) ||
                    !ParseInteger(tokens[6], &instruction.onClosedPc) ||
                    !ParseInteger(tokens[7], &instruction.onHolidayPc)) {
                    return Fail(error, "V2 SCHEDULE instruction is invalid");
                }
                instruction.type = RevisionInstructionType::ScheduleBranch;
                instruction.scheduleId = tokens[4];
            } else if (type == "RETURN") {
                if (tokens.size() != 5 || !ParseInteger(tokens[4], &instruction.menuPc)) {
                    return Fail(error, "V2 RETURN instruction is invalid");
                }
                instruction.type = RevisionInstructionType::ReturnToMenu;
            } else if (type == "RECORD") {
                if (config.schemaVersion < 3 || tokens.size() != 8 ||
                    !ParseInteger(tokens[4], &instruction.maximumDurationMilliseconds) ||
                    instruction.maximumDurationMilliseconds < 10'000 ||
                    instruction.maximumDurationMilliseconds > 180'000 ||
                    (tokens[5] != "-" &&
                     (tokens[5].size() != 1 ||
                      std::string("0123456789*#").find(tokens[5][0]) == std::string::npos)) ||
                    !ParseInteger(tokens[6], &instruction.nextPc) ||
                    !ParseInteger(tokens[7], &instruction.onUnavailablePc)) {
                    return Fail(error, "V3 RECORD instruction is invalid");
                }
                instruction.type = RevisionInstructionType::RecordMessage;
                instruction.finishKey = tokens[5] == "-" ? 0 : tokens[5][0];
            } else if (type == "EXTERNAL_CALL") {
                if (config.schemaVersion != 4 || tokens.size() != 9 ||
                    !call_control::IsPhoneNumber(tokens[4]) ||
                    !ParseInteger(tokens[5], &instruction.answerTimeoutMilliseconds) ||
                    instruction.answerTimeoutMilliseconds < 5'000 ||
                    instruction.answerTimeoutMilliseconds > 120'000 ||
                    instruction.answerTimeoutMilliseconds % 1'000 != 0 ||
                    !ParseInteger(tokens[6], &instruction.onCompletedPc) ||
                    !ParseInteger(tokens[7], &instruction.onNotConnectedPc) ||
                    !ParseInteger(tokens[8], &instruction.onSystemFailurePc)) {
                    return Fail(error, "V4 EXTERNAL_CALL instruction is invalid");
                }
                instruction.type = RevisionInstructionType::ExternalCall;
                instruction.externalNumber = tokens[4];
            } else if (type == "END") {
                if (tokens.size() != 4) return Fail(error, "V2 END instruction is invalid");
                instruction.type = RevisionInstructionType::EndCall;
            } else {
                return Fail(error, "V2 instruction operation is unsupported");
            }
            config.instructions.push_back(std::move(instruction));
        } else if (tokens[0] == "NODE") {
            if (config.schemaVersion != 1 || tokens.size() < 3 || config.nodes.size() >= kMaximumNodes ||
                !IsIdentifier(tokens[1]) || !nodeIds.insert(tokens[1]).second) {
                return Fail(error, "node is invalid or duplicated");
            }
            RevisionNode node;
            node.id = tokens[1];
            const std::string& type = tokens[2];
            if (type == "PLAY") {
                if (tokens.size() != 5 || !IsPromptIdentifier(tokens[3]) || !IsIdentifier(tokens[4])) return Fail(error, "PLAY node is invalid");
                node.type = RevisionNodeType::PlayPrompt; node.promptId = tokens[3]; node.next = tokens[4];
            } else if (type == "COLLECT") {
                uint32_t branchCount = 0;
                if (tokens.size() < 9 ||
                    (tokens[3] != "-" && !IsPromptIdentifier(tokens[3])) ||
                    !ParseInteger(tokens[4], &node.timeoutMilliseconds) || node.timeoutMilliseconds < 1000 || node.timeoutMilliseconds > 15000 ||
                    !ParseInteger(tokens[5], &node.maximumAttempts) || node.maximumAttempts < 1 || node.maximumAttempts > 3 ||
                    !IsIdentifier(tokens[6]) || !IsIdentifier(tokens[7]) ||
                    !ParseInteger(tokens[8], &branchCount) || branchCount < 1 || branchCount > 12 ||
                    tokens.size() != 9 + static_cast<size_t>(branchCount) * 2) {
                    return Fail(error, "COLLECT node is invalid");
                }
                node.type = RevisionNodeType::CollectDigit;
                node.promptId = tokens[3] == "-" ? "" : tokens[3];
                node.onTimeout = tokens[6]; node.onInvalid = tokens[7];
                for (uint32_t branch = 0; branch < branchCount; ++branch) {
                    const std::string& digit = tokens[9 + branch * 2];
                    const std::string& target = tokens[10 + branch * 2];
                    if (digit.size() != 1 || std::string("0123456789*#").find(digit[0]) == std::string::npos ||
                        !IsIdentifier(target) || !node.digitBranches.emplace(digit[0], target).second) {
                        return Fail(error, "COLLECT branch is invalid or duplicated");
                    }
                }
            } else if (type == "SCHEDULE") {
                if (tokens.size() != 7 || !IsIdentifier(tokens[3]) || !IsIdentifier(tokens[4]) || !IsIdentifier(tokens[5]) || !IsIdentifier(tokens[6])) return Fail(error, "SCHEDULE node is invalid");
                node.type = RevisionNodeType::ScheduleBranch; node.scheduleId = tokens[3]; node.onOpen = tokens[4]; node.onClosed = tokens[5]; node.onHoliday = tokens[6];
            } else if (type == "REPEAT") {
                if (tokens.size() != 6 || !IsIdentifier(tokens[3]) ||
                    !ParseInteger(tokens[4], &node.maximumRepeats) || node.maximumRepeats < 1 || node.maximumRepeats > 3 || !IsIdentifier(tokens[5])) return Fail(error, "REPEAT node is invalid");
                node.type = RevisionNodeType::RepeatMenu; node.repeatTarget = tokens[3]; node.onExhausted = tokens[5];
            } else if (type == "END") {
                if (tokens.size() != 3) return Fail(error, "END node is invalid");
                node.type = RevisionNodeType::EndCall;
            } else {
                return Fail(error, "node type is unsupported");
            }
            config.nodes.push_back(std::move(node));
        } else {
            return Fail(error, "configuration directive is unsupported");
        }
    }

    const bool bodyComplete = config.schemaVersion == 1
        ? rootSeen && !config.nodes.empty() && !sourceSeen && !programSeen && !entrySeen
        : sourceSeen && programSeen && entrySeen && !rootSeen && config.nodes.empty() &&
            !config.instructions.empty() &&
            (config.schemaVersion >= 3 ? maximumSessionSeen : !maximumSessionSeen);
    if (!header || !ended || !revisionSeen || !manifestSeen || !horizonSeen || !bodyComplete) {
        return Fail(error, "configuration is incomplete");
    }
    for (const ScheduleWindow& window : config.windows) {
        if (scheduleIds.count(window.scheduleId) == 0 ||
            window.startEpochSeconds < config.horizonStartEpochSeconds ||
            window.endEpochSeconds > config.horizonEndEpochSeconds) {
            return Fail(error, "schedule window is outside its declared schedule or horizon");
        }
    }
    for (const RevisionNode& node : config.nodes) {
        if ((!node.promptId.empty() && promptIds.count(node.promptId) == 0) ||
            (node.type == RevisionNodeType::ScheduleBranch && scheduleIds.count(node.scheduleId) == 0)) {
            return Fail(error, "node references a missing prompt or schedule");
        }
    }
    for (const RevisionInstruction& instruction : config.instructions) {
        if ((!instruction.promptId.empty() && promptIds.count(instruction.promptId) == 0) ||
            (instruction.type == RevisionInstructionType::ScheduleBranch &&
             scheduleIds.count(instruction.scheduleId) == 0)) {
            return Fail(error, "V2 instruction references a missing prompt or schedule");
        }
    }
    if (config.schemaVersion == 1) {
        if (!ValidateGraph(config, error)) return false;
    } else if (!ValidateProgramV2(config, error)) {
        return false;
    }
    *output = std::move(config);
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace ivrdroid
