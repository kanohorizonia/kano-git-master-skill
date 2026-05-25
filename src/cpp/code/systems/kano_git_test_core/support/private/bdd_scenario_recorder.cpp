#include "bdd_scenario_recorder.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kano::git::tests::functional {

auto BuildScenarioRecorderJson(const ScenarioRecorder& InRecorder) -> std::string;

namespace {

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string escaped;
    escaped.reserve(InValue.size() + 8);
    for (const unsigned char ch : InValue) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(digits[(ch >> 4) & 0x0f]);
                escaped.push_back(digits[ch & 0x0f]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

auto JsonString(const std::string& InValue) -> std::string {
    return "\"" + JsonEscape(InValue) + "\"";
}

auto JsonStringArray(const std::vector<std::string>& InValues) -> std::string {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < InValues.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << JsonString(InValues[i]);
    }
    out << "]";
    return out.str();
}

auto SafeFilenameStem(const std::string& InValue) -> std::string {
    std::string safe;
    safe.reserve(InValue.size());
    for (const unsigned char ch : InValue) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.') {
            safe.push_back(static_cast<char>(ch));
        } else {
            safe.push_back('_');
        }
    }
    return safe.empty() ? "scenario" : safe;
}

auto ResolveMetadataDir() -> std::filesystem::path {
    if (const char* explicitDir = std::getenv("KANO_BDD_METADATA_DIR"); explicitDir != nullptr && explicitDir[0] != '\0') {
        return std::filesystem::path(explicitDir).lexically_normal();
    }
    return (std::filesystem::current_path() / ".kano" / "tmp" / "test-metadata" / "bdd").lexically_normal();
}

auto CurrentExecutableFilename() -> std::string {
    if (const char* explicitName = std::getenv("KANO_TEST_BINARY_NAME"); explicitName != nullptr && explicitName[0] != '\0') {
        return explicitName;
    }
#if defined(_WIN32)
    std::string buffer(MAX_PATH, '\0');
    const auto written = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written > 0) {
        buffer.resize(written);
        return std::filesystem::path(buffer).filename().string();
    }
#endif
    return "unknown-test-binary";
}

auto ReplaceWithTempFile(const std::filesystem::path& InTempPath, const std::filesystem::path& InFinalPath) -> void {
#if defined(_WIN32)
    if (MoveFileExA(InTempPath.string().c_str(),
                    InFinalPath.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        throw std::runtime_error("failed to rename BDD scenario metadata temp file");
    }
#else
    std::filesystem::rename(InTempPath, InFinalPath);
#endif
}

} // namespace

ScenarioRecorder::ScenarioRecorder(std::string InScenarioId,
                                   std::string InFeature,
                                   std::string InTitle,
                                   std::string InSourceTestName)
    : scenarioId(std::move(InScenarioId)),
      feature(std::move(InFeature)),
      scenarioTitle(std::move(InTitle)),
      sourceTestName(std::move(InSourceTestName)) {}

ScenarioRecorder::~ScenarioRecorder() noexcept {
    try {
        WriteJsonSidecar();
    } catch (...) {
    }
}

auto ScenarioRecorder::Given(std::string InText) -> ScenarioRecorder& {
    return AddStep("Given", "given", std::move(InText));
}

auto ScenarioRecorder::AndGiven(std::string InText) -> ScenarioRecorder& {
    return AddStep("And", "given", std::move(InText));
}

auto ScenarioRecorder::When(std::string InText) -> ScenarioRecorder& {
    return AddStep("When", "when", std::move(InText));
}

auto ScenarioRecorder::AndWhen(std::string InText) -> ScenarioRecorder& {
    return AddStep("And", "when", std::move(InText));
}

auto ScenarioRecorder::Then(std::string InText) -> ScenarioRecorder& {
    return AddStep("Then", "then", std::move(InText));
}

auto ScenarioRecorder::AndThen(std::string InText) -> ScenarioRecorder& {
    return AddStep("And", "then", std::move(InText));
}

auto ScenarioRecorder::SetLayer(std::string InLayer) -> ScenarioRecorder& {
    layer = std::move(InLayer);
    return *this;
}

auto ScenarioRecorder::SetFeatured(const bool InFeatured) -> ScenarioRecorder& {
    featured = InFeatured;
    return *this;
}

auto ScenarioRecorder::SetDocVisibility(std::string InDocVisibility) -> ScenarioRecorder& {
    docVisibility = std::move(InDocVisibility);
    return *this;
}

auto ScenarioRecorder::SetAutomationStatus(std::string InAutomationStatus) -> ScenarioRecorder& {
    automationStatus = std::move(InAutomationStatus);
    return *this;
}

auto ScenarioRecorder::SetDiagramType(std::string InDiagramType) -> ScenarioRecorder& {
    diagramType = std::move(InDiagramType);
    return *this;
}

auto ScenarioRecorder::AddTag(std::string InTag) -> ScenarioRecorder& {
    tags.push_back(std::move(InTag));
    return *this;
}

auto ScenarioRecorder::AddActor(std::string InActor) -> ScenarioRecorder& {
    actors.push_back(std::move(InActor));
    return *this;
}

auto ScenarioRecorder::AddTrace(std::string InTrace) -> ScenarioRecorder& {
    traces.push_back(std::move(InTrace));
    return *this;
}

auto ScenarioRecorder::AddStep(std::string InKeyword, std::string InPhase, std::string InText) -> ScenarioRecorder& {
    steps.push_back(Step{.keyword = std::move(InKeyword), .phase = std::move(InPhase), .text = std::move(InText)});
    return *this;
}

auto ScenarioRecorder::WriteJsonSidecar() const -> void {
    const auto metadataDir = ResolveMetadataDir();
    std::filesystem::create_directories(metadataDir);

    const auto finalPath = (metadataDir / (SafeFilenameStem(scenarioId) + ".json")).lexically_normal();
    const auto tempPath = finalPath.string() + ".tmp";

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        out << BuildScenarioRecorderJson(*this);
        out.flush();
        if (!out.good()) {
            throw std::runtime_error("failed to write BDD scenario metadata");
        }
    }

    ReplaceWithTempFile(tempPath, finalPath);
}

auto BuildScenarioRecorderJson(const ScenarioRecorder& InRecorder) -> std::string {
    std::ostringstream out;
    out << "{\n";
    out << "  \"style\": \"bdd\",\n";
    out << "  \"layer\": " << JsonString(InRecorder.layer) << ",\n";
    out << "  \"feature\": " << JsonString(InRecorder.feature) << ",\n";
    out << "  \"scenarioId\": " << JsonString(InRecorder.scenarioId) << ",\n";
    out << "  \"scenarioTitle\": " << JsonString(InRecorder.scenarioTitle) << ",\n";
    out << "  \"featured\": " << (InRecorder.featured ? "true" : "false") << ",\n";
    out << "  \"docVisibility\": " << JsonString(InRecorder.docVisibility) << ",\n";
    out << "  \"automationStatus\": " << JsonString(InRecorder.automationStatus) << ",\n";
    out << "  \"diagramType\": " << JsonString(InRecorder.diagramType) << ",\n";
    out << "  \"sourceTestName\": " << JsonString(InRecorder.sourceTestName) << ",\n";
    out << "  \"sourceTestBinary\": " << JsonString(CurrentExecutableFilename()) << ",\n";
    out << "  \"tags\": " << JsonStringArray(InRecorder.tags) << ",\n";
    out << "  \"steps\": [\n";
    for (std::size_t i = 0; i < InRecorder.steps.size(); ++i) {
        const auto& step = InRecorder.steps[i];
        out << "    {\"keyword\": " << JsonString(step.keyword)
            << ", \"phase\": " << JsonString(step.phase)
            << ", \"text\": " << JsonString(step.text) << "}";
        if (i + 1 < InRecorder.steps.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"actors\": " << JsonStringArray(InRecorder.actors) << ",\n";
    out << "  \"traces\": " << JsonStringArray(InRecorder.traces) << ",\n";
    out << "  \"relatedArtifacts\": [],\n";
    out << "  \"environment\": {\"cwd\": " << JsonString(std::filesystem::current_path().generic_string()) << "},\n";
    out << "  \"lane\": \"functional\",\n";
    out << "  \"project\": \"kano-git\",\n";
    out << "  \"domain\": \"git\"\n";
    out << "}\n";
    return out.str();
}

} // namespace kano::git::tests::functional
