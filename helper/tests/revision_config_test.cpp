#include "revision_config.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char kHash[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char kPrompt[] = "11111111-1111-1111-1111-111111111111";

std::string ValidConfig() {
    return
        "IVRDROID_CONFIG_V1\n"
        "REVISION 7\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "SCHEDULE work\n"
        "WINDOW work O 1700000100 1700000200\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "ROOT welcome\n"
        "NODE welcome PLAY " + std::string(kPrompt) + " menu\n"
        "NODE menu COLLECT - 5000 2 done done 2 1 repeat 2 done\n"
        "NODE repeat REPEAT welcome 2 done\n"
        "NODE done END\n"
        "END_CONFIG\n";
}

std::string Block(unsigned int value) {
    char block[37] = {};
    std::snprintf(block, sizeof(block), "00000000-0000-4000-8000-%012x", value);
    return block;
}

std::string ValidConfigV2() {
    return
        "IVRDROID_CONFIG_V2\n"
        "REVISION 8\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "INSTRUCTION 0 " + Block(1) + " COLLECT - 5000 3 2 5 6 7 2 1 1 2 3\n"
        "INSTRUCTION 1 " + Block(2) + " PLAY " + kPrompt + " 2\n"
        "INSTRUCTION 2 " + Block(3) + " RETURN 0\n"
        "INSTRUCTION 3 " + Block(4) + " PLAY " + kPrompt + " 4\n"
        "INSTRUCTION 4 " + Block(5) + " END\n"
        "INSTRUCTION 5 " + Block(6) + " END\n"
        "INSTRUCTION 6 " + Block(7) + " END\n"
        "INSTRUCTION 7 " + Block(8) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string ValidConfigV3() {
    return
        "IVRDROID_CONFIG_V3\n"
        "REVISION 9\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "MAX_SESSION 60500\n"
        "INSTRUCTION 0 " + Block(10) + " PLAY " + kPrompt + " 1\n"
        "INSTRUCTION 1 " + Block(11) + " RECORD 60000 # 2 3\n"
        "INSTRUCTION 2 " + Block(12) + " END\n"
        "INSTRUCTION 3 " + Block(13) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string ValidConfigV4() {
    return
        "IVRDROID_CONFIG_V4\n"
        "REVISION 10\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "MAX_AUTOMATED_SESSION_MS 30500\n"
        "INSTRUCTION 0 " + Block(20) + " PLAY " + kPrompt + " 1\n"
        "INSTRUCTION 1 " + Block(21) +
            " EXTERNAL_CALL +982112345678 30000 2 3 4\n"
        "INSTRUCTION 2 " + Block(22) + " END\n"
        "INSTRUCTION 3 " + Block(23) + " END\n"
        "INSTRUCTION 4 " + Block(24) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string ValidConfigV41() {
    return
        "IVRDROID_CONFIG_V4\n"
        "REVISION 11\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "MAX_AUTOMATED_SESSION_MS 18000\n"
        "INSTRUCTION 0 " + Block(30) + " COLLECT " + kPrompt +
            " 5000 3 2 2 3 4 1 1 1 BARGE_IN\n"
        "INSTRUCTION 1 " + Block(31) + " END\n"
        "INSTRUCTION 2 " + Block(32) + " END\n"
        "INSTRUCTION 3 " + Block(33) + " END\n"
        "INSTRUCTION 4 " + Block(34) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string V4MenuNoticeExternal(bool prompted) {
    const std::string prompt = prompted ? kPrompt : "-";
    const std::string bargeIn = prompted ? " BARGE_IN" : "";
    return
        "IVRDROID_CONFIG_V4\n"
        "REVISION 12\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "MAX_AUTOMATED_SESSION_MS 61000\n"
        "INSTRUCTION 0 " + Block(40) + " COLLECT " + prompt +
            " 5000 3 2 5 6 7 1 0 1" + bargeIn + "\n"
        "INSTRUCTION 1 " + Block(41) +
            " EXTERNAL_CALL +982112345678 30000 2 3 4\n"
        "INSTRUCTION 2 " + Block(42) + " END\n"
        "INSTRUCTION 3 " + Block(43) + " END\n"
        "INSTRUCTION 4 " + Block(44) + " END\n"
        "INSTRUCTION 5 " + Block(45) + " END\n"
        "INSTRUCTION 6 " + Block(46) + " END\n"
        "INSTRUCTION 7 " + Block(47) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string V4MenuNoticeRecording(bool prompted) {
    const std::string prompt = prompted ? kPrompt : "-";
    return
        "IVRDROID_CONFIG_V4\n"
        "REVISION 13\n"
        "MANIFEST " + std::string(kHash) + "\n"
        "HORIZON 1700000000 1730000000\n"
        "PROMPT " + std::string(kPrompt) + " " + kHash + " 4096\n"
        "SOURCE bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "PROGRAM cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "MAX_AUTOMATED_SESSION_MS 61000\n"
        "INSTRUCTION 0 " + Block(50) + " COLLECT " + prompt +
            " 5000 3 2 4 5 6 1 0 1\n"
        "INSTRUCTION 1 " + Block(51) + " RECORD 60000 # 2 3\n"
        "INSTRUCTION 2 " + Block(52) + " END\n"
        "INSTRUCTION 3 " + Block(53) + " END\n"
        "INSTRUCTION 4 " + Block(54) + " END\n"
        "INSTRUCTION 5 " + Block(55) + " END\n"
        "INSTRUCTION 6 " + Block(56) + " END\n"
        "ENTRY 0\n"
        "END_CONFIG\n";
}

std::string V3MenuGreetingRecording() {
    std::string document = V4MenuNoticeRecording(true);
    document.replace(
        document.find("IVRDROID_CONFIG_V4"),
        std::string("IVRDROID_CONFIG_V4").size(),
        "IVRDROID_CONFIG_V3");
    document.replace(
        document.find("MAX_AUTOMATED_SESSION_MS"),
        std::string("MAX_AUTOMATED_SESSION_MS").size(),
        "MAX_SESSION");
    return document;
}

}  // namespace

int main(int argc, char** argv) {
    ivrdroid::RevisionConfig config;
    std::string error;
    assert(ivrdroid::ParseRevisionConfig(ValidConfig(), &config, &error));
    assert(config.revisionId == 7);
    assert(config.FindNode("menu") != nullptr);
    assert(config.FindPrompt(kPrompt) != nullptr);
    bool holiday = true;
    assert(config.IsScheduleOpen("work", 1700000150, &holiday));
    assert(!holiday);
    assert(!config.IsScheduleOpen("work", 1700000300, &holiday));

    std::string invalid = ValidConfig();
    invalid.replace(invalid.find("NODE done END"), 13, "NODE done PLAY " + std::string(kPrompt) + " welcome");
    assert(!ivrdroid::ParseRevisionConfig(invalid, &config, &error));

    assert(ivrdroid::ParseRevisionConfig(ValidConfigV2(), &config, &error));
    assert(config.schemaVersion == 2);
    assert(config.entryPc == 0);
    assert(config.FindInstruction(3) != nullptr);
    assert(config.FindInstruction(3)->blockId == Block(4));
    assert(config.FindInstruction(2)->type == ivrdroid::RevisionInstructionType::ReturnToMenu);

    std::string shared = ValidConfigV2();
    shared.replace(shared.find("2 1 1 2 3"), 9, "2 1 1 2 1");
    assert(!ivrdroid::ParseRevisionConfig(shared, &config, &error));

    std::string corruptReturn = ValidConfigV2();
    corruptReturn.replace(corruptReturn.find("RETURN 0"), 8, "RETURN 1");
    assert(!ivrdroid::ParseRevisionConfig(corruptReturn, &config, &error));

    std::string unboundedReturnLimit = ValidConfigV2();
    unboundedReturnLimit.replace(
        unboundedReturnLimit.find("INSTRUCTION 7 " + Block(8) + " END"),
        std::string("INSTRUCTION 7 " + Block(8) + " END").size(),
        "INSTRUCTION 7 " + Block(8) + " RETURN 0");
    assert(!ivrdroid::ParseRevisionConfig(unboundedReturnLimit, &config, &error));

    assert(ivrdroid::ParseRevisionConfig(ValidConfigV3(), &config, &error));
    assert(config.schemaVersion == 3);
    assert(config.maximumSessionMilliseconds == 60'500);
    assert(config.FindInstruction(1)->type == ivrdroid::RevisionInstructionType::RecordMessage);
    assert(config.FindInstruction(1)->finishKey == '#');

    std::string noGreeting = ValidConfigV3();
    const std::string greeting = "INSTRUCTION 0 " + Block(10) + " PLAY " + kPrompt + " 1";
    noGreeting.replace(
        noGreeting.find(greeting),
        greeting.size(),
        "INSTRUCTION 0 " + Block(10) + " RECORD 60000 - 1 3");
    assert(!ivrdroid::ParseRevisionConfig(noGreeting, &config, &error));

    assert(!ivrdroid::ParseRevisionConfig(V3MenuGreetingRecording(), &config, &error));
    assert(error == "RECORD must immediately follow PLAY");

    std::string excessive = ValidConfigV3();
    excessive.replace(excessive.find("MAX_SESSION 60500"), 17, "MAX_SESSION 600001");
    assert(!ivrdroid::ParseRevisionConfig(excessive, &config, &error));

    assert(ivrdroid::ParseRevisionConfig(ValidConfigV4(), &config, &error));
    assert(config.schemaVersion == 4);
    const auto* external = config.FindInstruction(1);
    assert(external != nullptr);
    assert(external->type == ivrdroid::RevisionInstructionType::ExternalCall);
    assert(external->externalNumber == "+982112345678");
    assert(external->answerTimeoutMilliseconds == 30'000);
    assert(external->onCompletedPc == 2);
    assert(external->onNotConnectedPc == 3);
    assert(external->onSystemFailurePc == 4);

    std::string legacyMaximumInV4 = ValidConfigV4();
    legacyMaximumInV4.replace(
        legacyMaximumInV4.find("MAX_AUTOMATED_SESSION_MS"),
        std::string("MAX_AUTOMATED_SESSION_MS").size(),
        "MAX_SESSION");
    assert(!ivrdroid::ParseRevisionConfig(legacyMaximumInV4, &config, &error));

    std::string v4MaximumInV3 = ValidConfigV3();
    v4MaximumInV3.replace(
        v4MaximumInV3.find("MAX_SESSION"),
        std::string("MAX_SESSION").size(),
        "MAX_AUTOMATED_SESSION_MS");
    assert(!ivrdroid::ParseRevisionConfig(v4MaximumInV3, &config, &error));

    std::string externalWithoutNotice = ValidConfigV4();
    const std::string notice =
        "INSTRUCTION 0 " + Block(20) + " PLAY " + kPrompt + " 1";
    externalWithoutNotice.replace(
        externalWithoutNotice.find(notice),
        notice.size(),
        "INSTRUCTION 0 " + Block(20) +
            " EXTERNAL_CALL +982112345678 30000 1 3 4");
    assert(!ivrdroid::ParseRevisionConfig(externalWithoutNotice, &config, &error));

    assert(ivrdroid::ParseRevisionConfig(V4MenuNoticeExternal(true), &config, &error));
    assert(config.FindInstruction(0)->type == ivrdroid::RevisionInstructionType::CollectDigit);
    assert(config.FindInstruction(1)->type == ivrdroid::RevisionInstructionType::ExternalCall);

    assert(!ivrdroid::ParseRevisionConfig(V4MenuNoticeExternal(false), &config, &error));
    assert(error ==
        "External call requires an earlier Play prompt or menu prompt notice on this path.");

    assert(ivrdroid::ParseRevisionConfig(V4MenuNoticeRecording(true), &config, &error));
    assert(config.FindInstruction(1)->type == ivrdroid::RevisionInstructionType::RecordMessage);

    assert(!ivrdroid::ParseRevisionConfig(V4MenuNoticeRecording(false), &config, &error));
    assert(error ==
        "Record message requires an earlier Play prompt or menu prompt notice on this path.");

    std::string invalidExternalNumber = ValidConfigV4();
    invalidExternalNumber.replace(
        invalidExternalNumber.find("+982112345678"),
        std::string("+982112345678").size(),
        "+98211A345678");
    assert(!ivrdroid::ParseRevisionConfig(invalidExternalNumber, &config, &error));

    std::string nationalExternalNumber = ValidConfigV4();
    nationalExternalNumber.erase(nationalExternalNumber.find("+982112345678"), 1);
    assert(ivrdroid::ParseRevisionConfig(nationalExternalNumber, &config, &error));

    std::string excessiveAnswerTimeout = ValidConfigV4();
    excessiveAnswerTimeout.replace(
        excessiveAnswerTimeout.find("30000 2 3 4"),
        std::string("30000 2 3 4").size(),
        "120001 2 3 4");
    assert(!ivrdroid::ParseRevisionConfig(excessiveAnswerTimeout, &config, &error));

    std::string fractionalAnswerTimeout = ValidConfigV4();
    fractionalAnswerTimeout.replace(
        fractionalAnswerTimeout.find("30000 2 3 4"),
        std::string("30000 2 3 4").size(),
        "30500 2 3 4");
    assert(!ivrdroid::ParseRevisionConfig(fractionalAnswerTimeout, &config, &error));

    assert(ivrdroid::ParseRevisionConfig(ValidConfigV41(), &config, &error));
    const auto* interruptible = config.FindInstruction(0);
    assert(interruptible != nullptr);
    assert(interruptible->allowPromptBargeIn);

    std::string markerInV3 = ValidConfigV41();
    markerInV3.replace(
        markerInV3.find("IVRDROID_CONFIG_V4"),
        std::string("IVRDROID_CONFIG_V4").size(),
        "IVRDROID_CONFIG_V3");
    markerInV3.replace(
        markerInV3.find("MAX_AUTOMATED_SESSION_MS"),
        std::string("MAX_AUTOMATED_SESSION_MS").size(),
        "MAX_SESSION");
    assert(!ivrdroid::ParseRevisionConfig(markerInV3, &config, &error));

    std::string markerWithoutPrompt = ValidConfigV41();
    markerWithoutPrompt.replace(
        markerWithoutPrompt.find("COLLECT " + std::string(kPrompt)),
        std::string("COLLECT " + std::string(kPrompt)).size(),
        "COLLECT -");
    assert(!ivrdroid::ParseRevisionConfig(markerWithoutPrompt, &config, &error));

    std::string invalidMarker = ValidConfigV41();
    invalidMarker.replace(
        invalidMarker.find("BARGE_IN"),
        std::string("BARGE_IN").size(),
        "INTERRUPT");
    assert(!ivrdroid::ParseRevisionConfig(invalidMarker, &config, &error));

    assert(argc == 4);
    std::ifstream fixture(argv[1]);
    std::ostringstream document;
    document << fixture.rdbuf();
    assert(fixture.good() || fixture.eof());
    assert(ivrdroid::ParseRevisionConfig(document.str(), &config, &error));
    assert(config.schemaVersion == 3);
    assert(config.revisionId == 85);
    assert(config.sourceSha256 ==
        "fbf8eecfeff3296e506d7921d15f50ab2526489d995dff1c3dfc50cc18ab8102");
    assert(config.programSha256 ==
        "e83d54c4f864b2b6e8a7acae50e6119438bd954a26ccd15267d56d9d8a4d6a65");
    assert(config.FindInstruction(1)->type == ivrdroid::RevisionInstructionType::RecordMessage);

    std::ifstream v4Fixture(argv[2]);
    std::ostringstream v4Document;
    v4Document << v4Fixture.rdbuf();
    assert(v4Fixture.good() || v4Fixture.eof());
    assert(ivrdroid::ParseRevisionConfig(v4Document.str(), &config, &error));
    assert(config.schemaVersion == 4);
    assert(config.revisionId == 86);
    assert(config.maximumSessionMilliseconds == 61'000);
    assert(config.sourceSha256 ==
        "fae9c50a514c5474b8834495fcb3bdd71bb308825362ca0ea2bb14c1ea7750de");
    assert(config.programSha256 ==
        "4dac380112c80705b7efe7ddc9c13f230b05f394b0b9a6bef73c72d83639cb3e");
    assert(config.FindInstruction(1)->type ==
        ivrdroid::RevisionInstructionType::ExternalCall);
    assert(config.FindInstruction(1)->externalNumber == "03136644636");

    std::ifstream v41Fixture(argv[3]);
    std::ostringstream v41Document;
    v41Document << v41Fixture.rdbuf();
    assert(v41Fixture.good() || v41Fixture.eof());
    assert(ivrdroid::ParseRevisionConfig(v41Document.str(), &config, &error));
    assert(config.schemaVersion == 4);
    assert(config.revisionId == 87);
    assert(config.maximumSessionMilliseconds == 21'000);
    assert(config.FindInstruction(0)->type ==
        ivrdroid::RevisionInstructionType::CollectDigit);
    assert(config.FindInstruction(0)->allowPromptBargeIn);
    return 0;
}
