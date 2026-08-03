#include <catch2/catch_test_macros.hpp>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "regression_coverage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace kano::git::commands::regression;

namespace kano::git::commands {
void RegisterRegression(CLI::App &InApp);
}

namespace {

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(const std::string &InLabel) {
    static unsigned long long sequence = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("kog-regression-coverage-" + InLabel + "-" +
             std::to_string(++sequence));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] auto Path() const -> const std::filesystem::path & {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class ScopedEnvironment {
public:
  ScopedEnvironment(const char *InName, std::optional<std::string> InValue)
      : name_(InName) {
    if (const char *previous = std::getenv(InName); previous != nullptr) {
      previous_ = std::string(previous);
    }
    Set(std::move(InValue));
  }

  ~ScopedEnvironment() { Set(previous_); }

private:
  void Set(const std::optional<std::string> &InValue) {
#if defined(_WIN32)
    _putenv_s(name_.c_str(), InValue.has_value() ? InValue->c_str() : "");
#else
    if (InValue.has_value()) {
      setenv(name_.c_str(), InValue->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }

  std::string name_;
  std::optional<std::string> previous_;
};

struct CliResult {
  int exitCode = 0;
  std::string stdoutText;
  std::string stderrText;
};

auto InvokeRegressionCli(const std::vector<std::string> &InArguments)
    -> CliResult {
  CLI::App app{"test", "kano-git"};
  kano::git::commands::RegisterRegression(app);

  std::vector<std::string> arguments;
  arguments.reserve(InArguments.size() + 1);
  arguments.push_back("kano-git");
  arguments.insert(arguments.end(), InArguments.begin(), InArguments.end());
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  for (auto &argument : arguments) {
    argv.push_back(argument.data());
  }

  std::ostringstream stdoutCapture;
  std::ostringstream stderrCapture;
  auto *previousStdout = std::cout.rdbuf(stdoutCapture.rdbuf());
  auto *previousStderr = std::cerr.rdbuf(stderrCapture.rdbuf());

  CliResult result;
  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError &error) {
    result.exitCode = error.get_exit_code();
  }

  std::cout.rdbuf(previousStdout);
  std::cerr.rdbuf(previousStderr);
  result.stdoutText = stdoutCapture.str();
  result.stderrText = stderrCapture.str();
  return result;
}

auto RepoRoot() -> std::filesystem::path {
  return std::filesystem::weakly_canonical(
      std::filesystem::path(KANO_GIT_TEST_REPO_ROOT));
}

void WriteText(const std::filesystem::path &InPath, const std::string &InText) {
  std::filesystem::create_directories(InPath.parent_path());
  std::ofstream output(InPath,
                       std::ios::out | std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());
  output << InText;
}

auto ReadText(const std::filesystem::path &InPath) -> std::string {
  std::ifstream input(InPath, std::ios::in | std::ios::binary);
  REQUIRE(input.good());
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

auto IsIdentifierCharacter(const char InCharacter) -> bool {
  const auto character = static_cast<unsigned char>(InCharacter);
  return std::isalnum(character) != 0 || InCharacter == '_';
}

auto SkipQuotedLiteral(const std::string &InSource, std::size_t InQuotePosition,
                       char InQuote) -> std::size_t {
  for (std::size_t index = InQuotePosition + 1; index < InSource.size();
       ++index) {
    if (InSource[index] == '\\') {
      ++index;
    } else if (InSource[index] == InQuote) {
      return index + 1;
    }
  }
  return InSource.size();
}

auto SkipRawStringLiteral(const std::string &InSource,
                          std::size_t InRawPrefixPosition)
    -> std::optional<std::size_t> {
  if (InRawPrefixPosition + 1 >= InSource.size() ||
      InSource[InRawPrefixPosition] != 'R' ||
      InSource[InRawPrefixPosition + 1] != '"') {
    return std::nullopt;
  }
  const auto delimiterStart = InRawPrefixPosition + 2;
  const auto openingParenthesis = InSource.find('(', delimiterStart);
  if (openingParenthesis == std::string::npos ||
      openingParenthesis - delimiterStart > 16) {
    return std::nullopt;
  }
  const auto delimiter =
      InSource.substr(delimiterStart, openingParenthesis - delimiterStart);
  for (const unsigned char character : delimiter) {
    if (std::isspace(character) != 0 || character == '\\' || character == '(' ||
        character == ')') {
      return std::nullopt;
    }
  }
  const auto terminator = ")" + delimiter + "\"";
  const auto close = InSource.find(terminator, openingParenthesis + 1);
  return close == std::string::npos
             ? std::optional<std::size_t>{InSource.size()}
             : std::optional<std::size_t>{close + terminator.size()};
}

void SkipTrivia(const std::string &InSource, std::size_t &InOutPosition) {
  while (InOutPosition < InSource.size()) {
    if (std::isspace(static_cast<unsigned char>(InSource[InOutPosition])) !=
        0) {
      ++InOutPosition;
      continue;
    }
    if (InOutPosition + 1 < InSource.size() && InSource[InOutPosition] == '/' &&
        InSource[InOutPosition + 1] == '/') {
      const auto newline = InSource.find('\n', InOutPosition + 2);
      InOutPosition =
          newline == std::string::npos ? InSource.size() : newline + 1;
      continue;
    }
    if (InOutPosition + 1 < InSource.size() && InSource[InOutPosition] == '/' &&
        InSource[InOutPosition + 1] == '*') {
      const auto close = InSource.find("*/", InOutPosition + 2);
      InOutPosition = close == std::string::npos ? InSource.size() : close + 2;
      continue;
    }
    break;
  }
}

auto ParseOrdinaryStringLiteral(const std::string &InSource,
                                std::size_t InQuotePosition)
    -> std::optional<std::pair<std::string, std::size_t>> {
  if (InQuotePosition >= InSource.size() || InSource[InQuotePosition] != '"') {
    return std::nullopt;
  }
  std::string value;
  for (std::size_t index = InQuotePosition + 1; index < InSource.size();
       ++index) {
    const char character = InSource[index];
    if (character == '"') {
      return std::pair{value, index + 1};
    }
    if (character == '\\') {
      if (++index >= InSource.size()) {
        return std::nullopt;
      }
      switch (InSource[index]) {
      case '\\':
        value.push_back('\\');
        break;
      case '"':
        value.push_back('"');
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      default:
        value.push_back(InSource[index]);
        break;
      }
      continue;
    }
    value.push_back(character);
  }
  return std::nullopt;
}

auto HasExactCatchTestRegistration(const std::string &InSource,
                                   const std::string &InTestName) -> bool {
  constexpr std::string_view macroName = "TEST_CASE";
  for (std::size_t index = 0; index < InSource.size();) {
    if (InSource[index] == '/' && index + 1 < InSource.size() &&
        (InSource[index + 1] == '/' || InSource[index + 1] == '*')) {
      SkipTrivia(InSource, index);
      continue;
    }
    if (const auto rawEnd = SkipRawStringLiteral(InSource, index);
        rawEnd.has_value()) {
      index = *rawEnd;
      continue;
    }
    if (InSource[index] == '"' || InSource[index] == '\'') {
      index = SkipQuotedLiteral(InSource, index, InSource[index]);
      continue;
    }

    const bool macroMatches =
        index + macroName.size() <= InSource.size() &&
        InSource.compare(index, macroName.size(), macroName) == 0 &&
        (index == 0 || !IsIdentifierCharacter(InSource[index - 1])) &&
        (index + macroName.size() == InSource.size() ||
         !IsIdentifierCharacter(InSource[index + macroName.size()]));
    if (!macroMatches) {
      ++index;
      continue;
    }

    std::size_t position = index + macroName.size();
    SkipTrivia(InSource, position);
    if (position >= InSource.size() || InSource[position] != '(') {
      index += macroName.size();
      continue;
    }
    ++position;
    SkipTrivia(InSource, position);
    const auto literal = ParseOrdinaryStringLiteral(InSource, position);
    if (!literal.has_value()) {
      index += macroName.size();
      continue;
    }
    position = literal->second;
    SkipTrivia(InSource, position);
    if (literal->first == InTestName && position < InSource.size() &&
        (InSource[position] == ',' || InSource[position] == ')')) {
      return true;
    }
    index += macroName.size();
  }
  return false;
}

auto HasExactLine(const std::string &InText, const std::string &InExpectedLine)
    -> bool {
  std::istringstream lines(InText);
  for (std::string line; std::getline(lines, line);) {
    if (line == InExpectedLine) {
      return true;
    }
  }
  return false;
}

auto StrictFixture() -> nlohmann::json {
  return {
      {"schema", "kog.regression.incident-map.v1"},
      {"incidents",
       nlohmann::json::array(
           {{{"incident_id", "KG-BUG-0001"},
             {"incident_date", "2026-07-28"},
             {"root_cause", "A verified concrete regression cause."},
             {"backlog_ref", "KG-BUG-0001"},
             {"change_ref", "commit:strict-fixture"},
             {"workflow_contracts",
              nlohmann::json::array({"converges-or-fails"})},
             {"regression_cases",
              nlohmann::json::array(
                  {{{"case_id", "KG-BUG-0001/strict-case"},
                    {"test_name", "strict case"},
                    {"test_file", "src/tests/strict.cpp"},
                    {"test_kind", "unit"},
                    {"mapping_state", "source-linked"}}})}}})},
  };
}

} // namespace

TEST_CASE("dogfood incident manifest maps stable source cases without "
          "execution claims",
          "[unit][regression][coverage][KG-TSK-0052]") {
  const auto manifest = RepoRoot() / "assets" / "regression" / "incidents.json";
  const auto loaded = LoadCoverageManifest(manifest);

  INFO(loaded.error);
  REQUIRE(loaded.ok);
  REQUIRE(loaded.report.incidents.size() == 10);
  REQUIRE(loaded.report.gaps.empty());

  const auto auditIncident = std::find_if(
      loaded.report.incidents.begin(), loaded.report.incidents.end(),
      [](const Incident &InIncident) {
        return InIncident.incidentId == "KG-BUG-0088";
      });
  REQUIRE(auditIncident != loaded.report.incidents.end());
  REQUIRE(auditIncident->regressionCases.size() == 9);
  REQUIRE(std::any_of(
      auditIncident->regressionCases.begin(),
      auditIncident->regressionCases.end(), [](const RegressionCase &InCase) {
        return InCase.caseId == "KG-BUG-0088/adaptive-terminal-theme" &&
               InCase.testName ==
                   "TUI auto theme uses COLORFGBG when available and stays "
                   "adaptive when unknown";
      }));

  const auto asyncIncident = std::find_if(
      loaded.report.incidents.begin(), loaded.report.incidents.end(),
      [](const Incident &InIncident) {
        return InIncident.incidentId == "KG-BUG-0091";
      });
  REQUIRE(asyncIncident != loaded.report.incidents.end());
  REQUIRE(asyncIncident->regressionCases.size() == 9);
  REQUIRE(std::any_of(
      asyncIncident->regressionCases.begin(),
      asyncIncident->regressionCases.end(), [](const RegressionCase &InCase) {
        return InCase.caseId == "KG-BUG-0091/first-frame-before-startup-io" &&
               InCase.testName ==
                   "TUI first frame schedules startup I/O after "
                   "ScreenInteractive activation and handles q while pending";
      }));

  const auto credentialIncident = std::find_if(
      loaded.report.incidents.begin(), loaded.report.incidents.end(),
      [](const Incident &InIncident) {
        return InIncident.incidentId == "KG-BUG-0094";
      });
  REQUIRE(credentialIncident != loaded.report.incidents.end());
  REQUIRE(credentialIncident->regressionCases.size() == 2);

  std::size_t linkedCaseCount = 0;
  for (const auto &incident : loaded.report.incidents) {
    linkedCaseCount += incident.regressionCases.size();
  }
  REQUIRE(linkedCaseCount == 36);

  const auto text = RenderCoverageText(loaded.report);
  REQUIRE(text.find("execution_evidence=not-evaluated") != std::string::npos);
  REQUIRE(text.find("linked_cases=36") != std::string::npos);
  REQUIRE(text.find("passed") == std::string::npos);
  REQUIRE(text.find("executed") == std::string::npos);

  const auto json = RenderCoverageJson(loaded.report);
  REQUIRE(json.find("\"execution_evidence\": \"not-evaluated\"") !=
          std::string::npos);
  REQUIRE(json.find("\"linked_cases\": 36") != std::string::npos);
}

TEST_CASE("default registry paths and exact test names resolve to source",
          "[unit][regression][coverage][registry][KG-TSK-0052]") {
  const auto loaded = LoadCoverageManifest(RepoRoot() / "assets" /
                                           "regression" / "incidents.json");
  INFO(loaded.error);
  REQUIRE(loaded.ok);

  for (const auto &incident : loaded.report.incidents) {
    for (const auto &regressionCase : incident.regressionCases) {
      const auto sourcePath = RepoRoot() / regressionCase.testFile;
      INFO(regressionCase.caseId);
      INFO(sourcePath);
      REQUIRE(std::filesystem::is_regular_file(sourcePath));
      REQUIRE(HasExactCatchTestRegistration(ReadText(sourcePath),
                                            regressionCase.testName));
    }
  }
}

TEST_CASE(
    "exact Catch2 registration matching rejects suffix and prefix collisions",
    "[unit][regression][coverage][registry][KG-TSK-0052]") {
  const std::string source =
      R"(TEST_CASE("plan_file_clean_but_ahead_continues_to_push",
                     "[functional][commit-push][contract]") {})";

  REQUIRE(HasExactCatchTestRegistration(
      source, "plan_file_clean_but_ahead_continues_to_push"));
  REQUIRE_FALSE(HasExactCatchTestRegistration(
      source, "clean_but_ahead_continues_to_push"));
}

TEST_CASE("Catch2 registration scan ignores comments and fixture strings",
          "[unit][regression][coverage][registry][lexer][KG-TSK-0052]") {
  const std::string source = R"source(
// TEST_CASE("line comment decoy", "[fixture]") {}
/*
TEST_CASE("block comment decoy", "[fixture]") {}
*/
const char* normalFixture = "TEST_CASE(\"normal string decoy\", \"[fixture]\") {}";
const char* rawFixture = R"fixture(
TEST_CASE("raw string decoy", "[fixture]") {}
)fixture";
const char characterFixture = '"';

TEST_CASE(
    "real registered case",
    "[unit][real]") {}
)source";

  REQUIRE_FALSE(HasExactCatchTestRegistration(source, "line comment decoy"));
  REQUIRE_FALSE(HasExactCatchTestRegistration(source, "block comment decoy"));
  REQUIRE_FALSE(HasExactCatchTestRegistration(source, "normal string decoy"));
  REQUIRE_FALSE(HasExactCatchTestRegistration(source, "raw string decoy"));
  REQUIRE(HasExactCatchTestRegistration(source, "real registered case"));
}

TEST_CASE(
    "coverage gap is informational unless the explicit fail gate is enabled",
    "[unit][regression][coverage][KG-TSK-0052]") {
  TemporaryDirectory temp("gap");
  const auto manifest = temp.Path() / "incidents.json";
  WriteText(manifest,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A concrete dogfood failure has not been backfilled yet.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:gap-fixture",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    }
  ]
})");

  const auto loaded = LoadCoverageManifest(manifest);
  INFO(loaded.error);
  REQUIRE(loaded.ok);
  REQUIRE(loaded.report.gaps == std::vector<std::string>{"KG-BUG-0001"});
  REQUIRE(CoverageExitCode(loaded.report, false) == 0);
  REQUIRE(CoverageExitCode(loaded.report, true) == kCoverageGapExitCode);
}

TEST_CASE(
    "registered coverage command honors text json and exit-code contracts",
    "[functional][regression][coverage][cli][KG-TSK-0052]") {
  ScopedEnvironment skillRoot("KANO_GIT_SKILL_ROOT",
                              RepoRoot().generic_string());

  const auto textResult = InvokeRegressionCli({"regression", "coverage"});
  INFO(textResult.stderrText);
  REQUIRE(textResult.exitCode == 0);
  REQUIRE(textResult.stdoutText.find("KOG regression coverage\n") == 0);
  REQUIRE(textResult.stdoutText.find("gaps=0") != std::string::npos);

  const auto jsonResult = InvokeRegressionCli(
      {"regression", "coverage", "--format", "json", "--fail-on-gap"});
  INFO(jsonResult.stderrText);
  REQUIRE(jsonResult.exitCode == 0);
  REQUIRE(jsonResult.stdoutText.find(
              "\"schema\": \"kog.regression.coverage.v1\"") !=
          std::string::npos);
  REQUIRE(jsonResult.stdoutText.find("\"gaps\": 0") != std::string::npos);

  const auto invalidFormat =
      InvokeRegressionCli({"regression", "coverage", "--format", "yaml"});
  REQUIRE(invalidFormat.exitCode == kInvalidManifestExitCode);
  REQUIRE(invalidFormat.stderrText.find("invalid --format 'yaml'") !=
          std::string::npos);

  TemporaryDirectory temp("cli-gap");
  const auto gapManifest = temp.Path() / "incidents.json";
  WriteText(gapManifest,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A concrete regression case has not been linked.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:gap-cli-fixture",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    }
  ]
})");
  const auto gapResult =
      InvokeRegressionCli({"regression", "coverage", "--manifest",
                           gapManifest.generic_string(), "--fail-on-gap"});
  REQUIRE(gapResult.exitCode == kCoverageGapExitCode);
  REQUIRE(gapResult.stdoutText.find("gaps=1") != std::string::npos);
}

TEST_CASE("invalid manifests fail closed with exit code two",
          "[unit][regression][coverage][KG-TSK-0052]") {
  TemporaryDirectory temp("invalid");
  const auto malformed = temp.Path() / "malformed.json";
  WriteText(malformed, "{not-json\n");
  const auto malformedResult = LoadCoverageManifest(malformed);
  REQUIRE_FALSE(malformedResult.ok);
  REQUIRE(malformedResult.error.find("invalid JSON") != std::string::npos);

  const auto unknownField = temp.Path() / "unknown-field.json";
  WriteText(unknownField,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [],
  "typo": true
})");
  const auto unknownFieldResult = LoadCoverageManifest(unknownField);
  REQUIRE_FALSE(unknownFieldResult.ok);
  REQUIRE(unknownFieldResult.error == "manifest: unknown field 'typo'");

  CoverageReport report;
  REQUIRE(CoverageExitCode(report, false) == 0);
  REQUIRE(kInvalidManifestExitCode == 2);
}

TEST_CASE("template and unresolved placeholder values are rejected",
          "[unit][regression][coverage][strict][KG-TSK-0052]") {
  const auto templateResult = LoadCoverageManifest(
      RepoRoot() / "assets" / "regression" / "case-template.json");
  REQUIRE_FALSE(templateResult.ok);
  REQUIRE(templateResult.error.find("placeholder values are not allowed") !=
          std::string::npos);

  TemporaryDirectory temp("placeholder");
  const auto manifest = temp.Path() / "incidents.json";
  WriteText(manifest,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A verified root cause.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "pending:KG-BUG-0001",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    }
  ]
})");
  const auto placeholderResult = LoadCoverageManifest(manifest);
  REQUIRE_FALSE(placeholderResult.ok);
  REQUIRE(
      placeholderResult.error ==
      "manifest.incidents[0].change_ref: placeholder values are not allowed");

  const auto proseManifest = temp.Path() / "field-aware-prose.json";
  auto proseDocument = StrictFixture();
  proseDocument["incidents"][0]["root_cause"] =
      "A pending queue state exposed a template defect while a todo label "
      "remained prose.";
  WriteText(proseManifest, proseDocument.dump(2));
  const auto proseResult = LoadCoverageManifest(proseManifest);
  INFO(proseResult.error);
  REQUIRE(proseResult.ok);

  const std::vector<std::string> rootCauseSentinels = {
      "TODO: replace with verified cause",
      "State the verified technical cause, not only the observed symptom.",
  };
  for (std::size_t index = 0; index < rootCauseSentinels.size(); ++index) {
    const auto rootCauseManifest =
        temp.Path() /
        ("root-cause-sentinel-" + std::to_string(index) + ".json");
    auto rootCauseDocument = StrictFixture();
    rootCauseDocument["incidents"][0]["root_cause"] = rootCauseSentinels[index];
    WriteText(rootCauseManifest, rootCauseDocument.dump(2));
    const auto rootCauseResult = LoadCoverageManifest(rootCauseManifest);
    INFO(rootCauseSentinels[index]);
    REQUIRE_FALSE(rootCauseResult.ok);
    REQUIRE(
        rootCauseResult.error ==
        "manifest.incidents[0].root_cause: placeholder values are not allowed");
  }
}

TEST_CASE("required strings reject whitespace and non-printable values",
          "[unit][regression][coverage][strict][KG-TSK-0052]") {
  TemporaryDirectory temp("blank-values");

  struct InvalidField {
    const char *label;
    std::vector<std::string> path;
    std::string value;
    std::string expectedPath;
    std::string expectedMessage = "must be a non-blank printable string";
  };
  const std::vector<InvalidField> invalidFields = {
      {"root-cause",
       {"incidents", "0", "root_cause"},
       " \t ",
       "manifest.incidents[0].root_cause"},
      {"change-ref",
       {"incidents", "0", "change_ref"},
       "   ",
       "manifest.incidents[0].change_ref"},
      {"workflow-contract",
       {"incidents", "0", "workflow_contracts", "0"},
       " ",
       "manifest.incidents[0].workflow_contracts[0]",
       "expected non-blank printable string"},
      {"test-file",
       {"incidents", "0", "regression_cases", "0", "test_file"},
       "   ",
       "manifest.incidents[0].regression_cases[0].test_file"},
      {"control-character",
       {"incidents", "0", "root_cause"},
       "cause\ncontinued",
       "manifest.incidents[0].root_cause"},
  };

  for (const auto &field : invalidFields) {
    auto document = StrictFixture();
    nlohmann::json *value = &document;
    for (const auto &component : field.path) {
      if (!component.empty() && std::all_of(component.begin(), component.end(),
                                            [](const unsigned char character) {
                                              return std::isdigit(character) !=
                                                     0;
                                            })) {
        value = &(*value)[static_cast<std::size_t>(std::stoul(component))];
      } else {
        value = &(*value)[component];
      }
    }
    *value = field.value;

    const auto manifest = temp.Path() / (std::string(field.label) + ".json");
    WriteText(manifest, document.dump(2));
    const auto loaded = LoadCoverageManifest(manifest);
    INFO(field.label);
    INFO(loaded.error);
    REQUIRE_FALSE(loaded.ok);
    REQUIRE(loaded.error == field.expectedPath + ": " + field.expectedMessage);
  }
}

TEST_CASE("incident dates must be calendar-valid",
          "[unit][regression][coverage][strict][KG-TSK-0052]") {
  TemporaryDirectory temp("calendar-date");
  auto document = StrictFixture();
  document["incidents"][0]["incident_date"] = "2026-99-99";
  const auto manifest = temp.Path() / "invalid-date.json";
  WriteText(manifest, document.dump(2));

  const auto loaded = LoadCoverageManifest(manifest);
  REQUIRE_FALSE(loaded.ok);
  REQUIRE(loaded.error ==
          "manifest.incidents[0].incident_date: expected valid calendar date");
}

TEST_CASE("manifest ordering is deterministic",
          "[unit][regression][coverage][KG-TSK-0052]") {
  TemporaryDirectory temp("ordering");
  const auto manifest = temp.Path() / "incidents.json";
  WriteText(manifest,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0002",
      "incident_date": "2026-07-27",
      "root_cause": "Second source incident.",
      "backlog_ref": "KG-BUG-0002",
      "change_ref": "commit:two",
      "workflow_contracts": ["z-contract", "a-contract"],
      "regression_cases": [
        {
          "case_id": "KG-BUG-0002/z-case",
          "test_name": "z case",
          "test_file": "src/tests/z.cpp",
          "test_kind": "unit",
          "mapping_state": "source-linked"
        },
        {
          "case_id": "KG-BUG-0002/a-case",
          "test_name": "a case",
          "test_file": "src/tests/a.cpp",
          "test_kind": "unit",
          "mapping_state": "source-linked"
        }
      ]
    },
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "First source incident.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:one",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    }
  ]
})");

  const auto loaded = LoadCoverageManifest(manifest);
  INFO(loaded.error);
  REQUIRE(loaded.ok);
  REQUIRE(loaded.report.incidents[0].incidentId == "KG-BUG-0001");
  REQUIRE(loaded.report.incidents[1].incidentId == "KG-BUG-0002");
  REQUIRE(loaded.report.incidents[1].workflowContracts ==
          std::vector<std::string>{"a-contract", "z-contract"});
  REQUIRE(loaded.report.incidents[1].regressionCases[0].caseId ==
          "KG-BUG-0002/a-case");
}

TEST_CASE("duplicate incident and case ids fail closed",
          "[unit][regression][coverage][strict][KG-TSK-0052]") {
  TemporaryDirectory temp("duplicate-ids");
  const auto duplicateIncident = temp.Path() / "duplicate-incident.json";
  WriteText(duplicateIncident,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "First occurrence.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:first",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    },
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-27",
      "root_cause": "Duplicate occurrence.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:second",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": []
    }
  ]
})");
  const auto duplicateIncidentResult = LoadCoverageManifest(duplicateIncident);
  REQUIRE_FALSE(duplicateIncidentResult.ok);
  REQUIRE(duplicateIncidentResult.error.find(
              "duplicate incident id 'KG-BUG-0001'") != std::string::npos);

  const auto duplicateCase = temp.Path() / "duplicate-case.json";
  WriteText(duplicateCase,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A concrete duplicate-case fixture.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:duplicate-case",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": [
        {
          "case_id": "KG-BUG-0001/same-case",
          "test_name": "first case",
          "test_file": "src/tests/first.cpp",
          "test_kind": "unit",
          "mapping_state": "source-linked"
        },
        {
          "case_id": "KG-BUG-0001/same-case",
          "test_name": "second case",
          "test_file": "src/tests/second.cpp",
          "test_kind": "unit",
          "mapping_state": "source-linked"
        }
      ]
    }
  ]
})");
  const auto duplicateCaseResult = LoadCoverageManifest(duplicateCase);
  REQUIRE_FALSE(duplicateCaseResult.ok);
  REQUIRE(duplicateCaseResult.error.find(
              "duplicate case id 'KG-BUG-0001/same-case'") !=
          std::string::npos);
}

TEST_CASE("unsafe test paths and empty governance registries fail closed",
          "[unit][regression][coverage][strict][KG-TSK-0052]") {
  TemporaryDirectory temp("unsafe-paths");
  const std::vector<std::string> unsafePaths = {
      "/tmp/absolute-test.cpp",
      "src/tests/../outside.cpp",
      "C:/outside.cpp",
      "https://example.test/outside.cpp",
  };
  for (std::size_t index = 0; index < unsafePaths.size(); ++index) {
    const auto manifest =
        temp.Path() / ("unsafe-" + std::to_string(index) + ".json");
    WriteText(manifest, std::string{R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A concrete unsafe-path fixture.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:unsafe-path",
      "workflow_contracts": ["converges-or-fails"],
      "regression_cases": [
        {
          "case_id": "KG-BUG-0001/unsafe-path",
          "test_name": "unsafe path",
          "test_file": ")"} +
                            unsafePaths[index] +
                            R"(",
          "test_kind": "unit",
          "mapping_state": "source-linked"
        }
      ]
    }
  ]
})");
    const auto loaded = LoadCoverageManifest(manifest);
    INFO(unsafePaths[index]);
    REQUIRE_FALSE(loaded.ok);
    REQUIRE(loaded.error.find("expected normalized repo-relative path") !=
            std::string::npos);
  }

  const auto emptyContracts = temp.Path() / "empty-contracts.json";
  WriteText(emptyContracts,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A concrete marker fixture.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:empty-contracts",
      "workflow_contracts": [],
      "regression_cases": []
    }
  ]
})");
  const auto emptyContractsResult = LoadCoverageManifest(emptyContracts);
  REQUIRE_FALSE(emptyContractsResult.ok);
  REQUIRE(emptyContractsResult.error ==
          "manifest.incidents[0].workflow_contracts: must contain at least one "
          "value");

  const auto emptyIncidents = temp.Path() / "empty-incidents.json";
  WriteText(emptyIncidents,
            R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": []
})");
  const auto emptyIncidentsResult = LoadCoverageManifest(emptyIncidents);
  REQUIRE_FALSE(emptyIncidentsResult.ok);
  REQUIRE(emptyIncidentsResult.error ==
          "manifest.incidents: must contain at least one incident");
}

TEST_CASE(
    "Docker build context allowlists only required regression governance "
    "assets",
    "[unit][regression][coverage][packaging][docker-context][KG-TSK-0052]") {
  const auto dockerIgnore = ReadText(RepoRoot() / ".dockerignore");
  const std::vector<std::string> requiredRules = {
      "!/SKILL.md",
      "!/assets/",
      "/assets/**",
      "!/assets/regression/",
      "/assets/regression/**",
      "!/assets/regression/incidents.json",
      "!/assets/regression/case-template.json",
  };
  for (const auto &rule : requiredRules) {
    INFO(rule);
    REQUIRE(HasExactLine(dockerIgnore, rule));
  }
  REQUIRE_FALSE(HasExactLine(dockerIgnore, "!/assets/**"));
  REQUIRE(std::filesystem::is_regular_file(RepoRoot() / "assets" /
                                           "regression" / "incidents.json"));
  REQUIRE(std::filesystem::is_regular_file(
      RepoRoot() / "assets" / "regression" / "case-template.json"));
}
