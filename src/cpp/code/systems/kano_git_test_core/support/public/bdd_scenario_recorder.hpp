#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::tests::functional {

class ScenarioRecorder {
public:
    ScenarioRecorder(std::string InScenarioId,
                     std::string InFeature,
                     std::string InTitle,
                     std::string InSourceTestName);
    ~ScenarioRecorder() noexcept;

    ScenarioRecorder(const ScenarioRecorder&) = delete;
    auto operator=(const ScenarioRecorder&) -> ScenarioRecorder& = delete;
    ScenarioRecorder(ScenarioRecorder&&) = delete;
    auto operator=(ScenarioRecorder&&) -> ScenarioRecorder& = delete;

    auto Given(std::string InText) -> ScenarioRecorder&;
    auto AndGiven(std::string InText) -> ScenarioRecorder&;
    auto When(std::string InText) -> ScenarioRecorder&;
    auto AndWhen(std::string InText) -> ScenarioRecorder&;
    auto Then(std::string InText) -> ScenarioRecorder&;
    auto AndThen(std::string InText) -> ScenarioRecorder&;

    auto SetLayer(std::string InLayer) -> ScenarioRecorder&;
    auto SetFeatured(bool InFeatured) -> ScenarioRecorder&;
    auto SetDocVisibility(std::string InDocVisibility) -> ScenarioRecorder&;
    auto SetAutomationStatus(std::string InAutomationStatus) -> ScenarioRecorder&;
    auto SetDiagramType(std::string InDiagramType) -> ScenarioRecorder&;
    auto AddTag(std::string InTag) -> ScenarioRecorder&;
    auto AddActor(std::string InActor) -> ScenarioRecorder&;
    auto AddTrace(std::string InTrace) -> ScenarioRecorder&;

private:
    friend auto BuildScenarioRecorderJson(const ScenarioRecorder& InRecorder) -> std::string;

    struct Step {
        std::string keyword;
        std::string phase;
        std::string text;
    };

    auto AddStep(std::string InKeyword, std::string InPhase, std::string InText) -> ScenarioRecorder&;
    auto WriteJsonSidecar() const -> void;

    std::string scenarioId;
    std::string feature;
    std::string scenarioTitle;
    std::string sourceTestName;
    std::string layer = "functional";
    bool featured = false;
    std::string docVisibility = "internal";
    std::string automationStatus = "automated";
    std::string diagramType = "flowchart";
    std::vector<std::string> tags;
    std::vector<Step> steps;
    std::vector<std::string> actors;
    std::vector<std::string> traces;
};

} // namespace kano::git::tests::functional
