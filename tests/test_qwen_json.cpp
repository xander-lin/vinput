#include "qwen_json.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void checkEq(const std::string &got, const std::string &want,
             const std::string &what) {
    if (got != want) {
        failures++;
        std::cerr << "FAIL: " << what << "\n  got:  \"" << got << "\"\n"
                  << "  want: \"" << want << "\"\n";
    }
}

// Current generation response (qwen-audio-3.x / fun-asr-flash), words listed
// after the sentence text.
const char *kNewGenWordsAfterText =
    "{\"output\":{\"sentence\":{\"begin_time\":760,\"channel_id\":0,"
    "\"end_time\":3800,\"sentence_end\":true,\"sentence_id\":1,"
    "\"text\":\"Hello World，这里是阿里巴巴语音实验室。\","
    "\"words\":[{\"begin_time\":760,\"end_time\":1040,\"fixed\":true,"
    "\"punctuation\":\"\",\"text\":\"Hello\"},{\"begin_time\":1040,"
    "\"end_time\":1240,\"fixed\":true,\"punctuation\":\"，\","
    "\"text\":\" World\"}]},"
    "\"text\":\"Hello World，这里是阿里巴巴语音实验室。\"},"
    "\"usage\":{\"duration\":4},"
    "\"request_id\":\"40e0734d-096f-9ae3-86c1-a8c013287561\"}";

// Same schema with words before the sentence text (field order not
// guaranteed).
const char *kNewGenWordsBeforeText =
    "{\"output\":{\"sentence\":{\"words\":[{\"text\":\"你好\"}],"
    "\"text\":\"你好世界\"},\"text\":\"你好世界\"},\"request_id\":\"r\"}";

// Speaker diarization response: output.sentences array instead of sentence;
// top-level output.text must win over nested sentence texts.
const char *kNewGenDiarization =
    "{\"output\":{\"sentences\":[{\"speaker_id\":0,\"text\":\"第一句\"},"
    "{\"speaker_id\":1,\"text\":\"第二句\"}],"
    "\"text\":\"第一句第二句\"},\"request_id\":\"r2\"}";

// Legacy qwen3-asr-flash response.
const char *kLegacyResponse =
    "{\"output\":{\"choices\":[{\"finish_reason\":\"stop\",\"message\":"
    "{\"content\":[{\"text\":\"识别出的文本内容\"}],\"role\":\"assistant\"}}]},"
    "\"request_id\":\"legacy\",\"usage\":{\"input_tokens\":10}}";

void testTextExtraction() {
    checkEq(vinput::qwenExtractText(kNewGenWordsAfterText),
            "Hello World，这里是阿里巴巴语音实验室。",
            "new-gen text with words after text");
    checkEq(vinput::qwenExtractText(kNewGenWordsBeforeText), "你好世界",
            "new-gen text with words before text");
    checkEq(vinput::qwenExtractText(kNewGenDiarization), "第一句第二句",
            "new-gen diarization output.text wins");
    checkEq(vinput::qwenExtractText(kLegacyResponse), "识别出的文本内容",
            "legacy choices/content text");
    checkEq(vinput::qwenExtractText("{\"output\":{\"text\":\"普通话\"}}"),
            "普通话", "minimal new-gen response");
    checkEq(vinput::qwenExtractText(""), "", "empty body yields no text");
    checkEq(vinput::qwenExtractText("{\"output\":{}}"), "",
            "missing text yields empty");
}

void testEscapes() {
    // \u4f60\u597d = 你好
    checkEq(vinput::jsonUnescape("\\u4f60\\u597d"), "你好",
            "unicode escape");
    checkEq(vinput::jsonUnescape("a\\\"b"), "a\"b", "quote escape");
    checkEq(vinput::jsonUnescape("a\\\\b"), "a\\b", "backslash escape");
    checkEq(vinput::jsonUnescape("a\\nb"), "a\nb", "newline escape");
    // Surrogate pair U+4E2D U+D860? Use a valid pair: U+20BB7 (𠮷).
    checkEq(vinput::jsonUnescape("\\ud842\\udfb7"), "\xF0\xA0\xAE\xB7",
            "surrogate pair escape");
    checkEq(vinput::jsonEscape("a\"b\\c\n"), "a\\\"b\\\\c\\n",
            "jsonEscape round trip characters");
    checkEq(vinput::jsonUnescape(vinput::jsonEscape("引号\"反斜杠\\")),
            "引号\"反斜杠\\", "escape round trip");
}

void testTopLevelString() {
    // Nested same-name key must not shadow the outer one.
    std::string json = "{\"text\":\"outer\",\"words\":[{\"text\":\"inner\"}]}";
    size_t obj = json.find('{');
    checkEq(vinput::qjsonTopLevelString(json, obj, "text"), "outer",
            "top-level text beats nested word text");
    checkEq(vinput::qjsonTopLevelString(json, obj, "missing"), "",
            "missing key yields empty");
    // Value of the nested object.
    size_t words = vinput::qjsonTopLevelObject(json, obj, "words");
    check(words == std::string::npos, "words is an array, not an object");
    std::string json2 = "{\"sentence\":{\"text\":\"句\"},\"text\":\"总\"}";
    size_t obj2 = json2.find('{');
    checkEq(vinput::qjsonTopLevelString(json2, obj2, "text"), "总",
            "outer text preferred over sentence text");
    size_t sentence = vinput::qjsonTopLevelObject(json2, obj2, "sentence");
    check(sentence != std::string::npos, "sentence object found");
    checkEq(vinput::qjsonTopLevelString(json2, sentence, "text"), "句",
            "sentence text extracted from nested object");
}

void testRawValue() {
    std::string cfg =
        "{\"api_key\":\"sk-1\",\"language_hints\":[\"zh\",\"en\"],"
        "\"vocabulary\":{\"Vinput\":5,\"fcitx\":3},"
        "\"keep_dialect\":true,\"endpoint\":\"https://x/y\","
        "\"note\":\"contains } brace\"}";
    checkEq(vinput::qjsonRawValue(cfg, "language_hints"), "[\"zh\",\"en\"]",
            "raw array value");
    checkEq(vinput::qjsonRawValue(cfg, "vocabulary"),
            "{\"Vinput\":5,\"fcitx\":3}", "raw object value");
    checkEq(vinput::qjsonRawValue(cfg, "keep_dialect"), "true",
            "raw boolean value");
    checkEq(vinput::qjsonRawValue(cfg, "endpoint"), "\"https://x/y\"",
            "raw string keeps quotes");
    checkEq(vinput::qjsonRawValue(cfg, "api_key"), "\"sk-1\"",
            "raw api key");
    checkEq(vinput::qjsonRawValue(cfg, "note"), "\"contains } brace\"",
            "brace inside string does not terminate object");
    checkEq(vinput::qjsonRawValue(cfg, "absent"), "", "absent key");
    checkEq(vinput::qjsonStringValue(cfg, "api_key"), "sk-1",
            "string value without quotes");
    checkEq(vinput::qjsonStringValue(cfg, "endpoint"), "https://x/y",
            "string value for endpoint");
}

void testErrorDetail() {
    checkEq(vinput::qwenExtractErrorDetail(
                "{\"code\":\"InvalidApiKey\",\"message\":\"Invalid API-key\","
                "\"request_id\":\"r\"}"),
            "InvalidApiKey: Invalid API-key", "code+message detail");
    checkEq(vinput::qwenExtractErrorDetail("{\"message\":\"boom\"}"), "boom",
            "message-only detail");
    checkEq(vinput::qwenExtractErrorDetail(""), "", "empty body detail");
}

void testLegacyDetection() {
    check(vinput::qwenIsLegacyModel("qwen3-asr-flash"), "qwen3 mainline");
    check(vinput::qwenIsLegacyModel("qwen3-asr-flash-2025-09-08"),
          "qwen3 snapshot");
    check(!vinput::qwenIsLegacyModel("qwen-audio-3.1-asr-flash"),
          "new-gen is not legacy");
    check(!vinput::qwenIsLegacyModel("fun-asr-flash-2026-06-15"),
          "fun-asr is not legacy");
    check(!vinput::qwenIsLegacyModel("some-future-model"),
          "unknown models default to new style");

    check(vinput::qwenUsesLegacyRequest("qwen3-asr-flash", "auto"),
          "auto resolves legacy for qwen3");
    check(!vinput::qwenUsesLegacyRequest("qwen-audio-3.1-asr-flash", "auto"),
          "auto resolves new style for 3.1");
    check(vinput::qwenUsesLegacyRequest("qwen-audio-3.1-asr-flash", "legacy"),
          "explicit legacy override");
    check(!vinput::qwenUsesLegacyRequest("qwen3-asr-flash", "input_audio"),
          "explicit input_audio override");
}

} // namespace

int main() {
    testTextExtraction();
    testEscapes();
    testTopLevelString();
    testRawValue();
    testErrorDetail();
    testLegacyDetection();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "qwen_json: all checks passed\n";
    return 0;
}
