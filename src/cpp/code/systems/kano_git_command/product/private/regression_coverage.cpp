#include "regression_coverage.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace kano::git::commands::regression {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kManifestSchema = "kog.regression.incident-map.v1";
constexpr std::string_view kReportSchema = "kog.regression.coverage.v1";
constexpr std::string_view kExecutionEvidence = "not-evaluated";

auto ToLower(std::string InValue) -> std::string {
  std::transform(InValue.begin(), InValue.end(), InValue.begin(),
                 [](const unsigned char InChar) {
                   return static_cast<char>(std::tolower(InChar));
                 });
  return InValue;
}

auto Trim(std::string InValue) -> std::string {
  const auto first = InValue.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = InValue.find_last_not_of(" \t\r\n");
  return InValue.substr(first, last - first + 1);
}

auto IsNonBlankPrintable(const std::string &InValue) -> bool {
  bool hasNonWhitespace = false;
  for (const unsigned char character : InValue) {
    if (character < 0x20 || character == 0x7f) {
      return false;
    }
    if (!std::isspace(character)) {
      hasNonWhitespace = true;
    }
  }
  return hasNonWhitespace;
}

enum class PlaceholderPolicy {
  None,
  ExactSentinel,
  TemplateField,
};

auto IsExactPlaceholder(const std::string &InValue) -> bool {
  const auto value = ToLower(Trim(InValue));
  return value == "pending" || value.starts_with("pending:") ||
         value == "todo" || value.starts_with("todo:") || value == "tbd" ||
         value.starts_with("tbd:") || value == "yyyy-mm-dd" ||
         value == "stable-scenario-slug" ||
         value == "exact stable test case name" ||
         value == "state the verified technical cause, not only the observed "
                  "symptom." ||
         value == "commit-or-pending-item-reference";
}

auto IsUnresolvedPlaceholder(const std::string &InValue,
                             PlaceholderPolicy InPolicy) -> bool {
  if (InPolicy == PlaceholderPolicy::None) {
    return false;
  }
  if (IsExactPlaceholder(InValue)) {
    return true;
  }
  if (InPolicy == PlaceholderPolicy::ExactSentinel) {
    return false;
  }

  const auto value = ToLower(Trim(InValue));
  return value.find("stable-scenario-slug") != std::string::npos ||
         value.find("exact stable test case name") != std::string::npos ||
         value.find("state the verified technical cause") !=
             std::string::npos ||
         value.find("commit-or-pending-item-reference") != std::string::npos ||
         value.find('<') != std::string::npos ||
         value.find('>') != std::string::npos;
}

auto SetError(std::string &OutError, const std::string &InPath,
              const std::string &InMessage) -> bool {
  OutError = InPath + ": " + InMessage;
  return false;
}

auto HasExactKeys(const Json &InValue, const std::set<std::string> &InExpected,
                  const std::string &InPath, std::string &OutError) -> bool {
  if (!InValue.is_object()) {
    return SetError(OutError, InPath, "expected object");
  }
  for (const auto &[key, unused] : InValue.items()) {
    (void)unused;
    if (!InExpected.contains(key)) {
      return SetError(OutError, InPath, "unknown field '" + key + "'");
    }
  }
  for (const auto &key : InExpected) {
    if (!InValue.contains(key)) {
      return SetError(OutError, InPath, "missing required field '" + key + "'");
    }
  }
  return true;
}

auto ReadRequiredString(const Json &InValue, const char *InKey,
                        const std::string &InPath,
                        PlaceholderPolicy InPlaceholderPolicy,
                        std::string &OutValue, std::string &OutError) -> bool {
  const auto &value = InValue.at(InKey);
  if (!value.is_string()) {
    return SetError(OutError, InPath + "." + InKey, "expected string");
  }
  OutValue = value.get<std::string>();
  if (!IsNonBlankPrintable(OutValue)) {
    return SetError(OutError, InPath + "." + InKey,
                    "must be a non-blank printable string");
  }
  if (IsUnresolvedPlaceholder(OutValue, InPlaceholderPolicy)) {
    return SetError(OutError, InPath + "." + InKey,
                    "placeholder values are not allowed");
  }
  return true;
}

auto ReadUniqueStringArray(const Json &InValue, const char *InKey,
                           const std::string &InPath,
                           std::vector<std::string> &OutValues,
                           std::string &OutError) -> bool {
  const auto &values = InValue.at(InKey);
  if (!values.is_array()) {
    return SetError(OutError, InPath + "." + InKey, "expected array");
  }

  std::set<std::string> seen;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto &value = values[index];
    if (!value.is_string() ||
        !IsNonBlankPrintable(value.get_ref<const std::string &>())) {
      return SetError(OutError,
                      InPath + "." + InKey + "[" + std::to_string(index) + "]",
                      "expected non-blank printable string");
    }
    const auto text = value.get<std::string>();
    if (IsUnresolvedPlaceholder(text, PlaceholderPolicy::TemplateField)) {
      return SetError(OutError,
                      InPath + "." + InKey + "[" + std::to_string(index) + "]",
                      "placeholder values are not allowed");
    }
    if (!seen.insert(text).second) {
      return SetError(OutError, InPath + "." + InKey,
                      "duplicate value '" + text + "'");
    }
    OutValues.push_back(text);
  }
  if (OutValues.empty()) {
    return SetError(OutError, InPath + "." + InKey,
                    "must contain at least one value");
  }
  std::sort(OutValues.begin(), OutValues.end());
  return true;
}

auto IsSafeRepoRelativePath(const std::string &InValue) -> bool {
  if (InValue.empty() || InValue.find('\\') != std::string::npos ||
      InValue.find(':') != std::string::npos || InValue.starts_with("~")) {
    return false;
  }
  const std::filesystem::path path(InValue);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto &component : path) {
    if (component == "." || component == ".." || component.empty()) {
      return false;
    }
  }
  return path.generic_string() == InValue;
}

auto ParseRegressionCase(const Json &InValue, const std::string &InIncidentId,
                         const std::string &InPath,
                         std::set<std::string> &InOutCaseIds,
                         RegressionCase &OutCase, std::string &OutError)
    -> bool {
  static const std::set<std::string> expectedKeys = {
      "case_id", "mapping_state", "test_file", "test_kind", "test_name",
  };
  if (!HasExactKeys(InValue, expectedKeys, InPath, OutError) ||
      !ReadRequiredString(InValue, "case_id", InPath,
                          PlaceholderPolicy::TemplateField, OutCase.caseId,
                          OutError) ||
      !ReadRequiredString(InValue, "test_name", InPath,
                          PlaceholderPolicy::TemplateField, OutCase.testName,
                          OutError) ||
      !ReadRequiredString(InValue, "test_file", InPath,
                          PlaceholderPolicy::TemplateField, OutCase.testFile,
                          OutError) ||
      !ReadRequiredString(InValue, "test_kind", InPath, PlaceholderPolicy::None,
                          OutCase.testKind, OutError) ||
      !ReadRequiredString(InValue, "mapping_state", InPath,
                          PlaceholderPolicy::None, OutCase.mappingState,
                          OutError)) {
    return false;
  }

  if (!OutCase.caseId.starts_with(InIncidentId + "/")) {
    return SetError(OutError, InPath + ".case_id",
                    "must start with incident id and '/'");
  }
  if (!InOutCaseIds.insert(OutCase.caseId).second) {
    return SetError(OutError, InPath + ".case_id",
                    "duplicate case id '" + OutCase.caseId + "'");
  }
  if (!IsSafeRepoRelativePath(OutCase.testFile)) {
    return SetError(OutError, InPath + ".test_file",
                    "expected normalized repo-relative path");
  }
  static const std::set<std::string> allowedKinds = {
      "contract", "e2e", "functional", "integration", "unit",
  };
  if (!allowedKinds.contains(OutCase.testKind)) {
    return SetError(OutError, InPath + ".test_kind",
                    "unsupported test kind '" + OutCase.testKind + "'");
  }
  if (OutCase.mappingState != "source-linked") {
    return SetError(OutError, InPath + ".mapping_state",
                    "must be 'source-linked'; execution status is "
                    "intentionally not inferred");
  }
  return true;
}

auto ParseIncident(const Json &InValue, const std::string &InPath,
                   std::set<std::string> &InOutIncidentIds,
                   std::set<std::string> &InOutCaseIds, Incident &OutIncident,
                   std::string &OutError) -> bool {
  static const std::set<std::string> expectedKeys = {
      "backlog_ref",      "change_ref", "incident_date",      "incident_id",
      "regression_cases", "root_cause", "workflow_contracts",
  };
  if (!HasExactKeys(InValue, expectedKeys, InPath, OutError) ||
      !ReadRequiredString(InValue, "incident_id", InPath,
                          PlaceholderPolicy::None, OutIncident.incidentId,
                          OutError) ||
      !ReadRequiredString(InValue, "incident_date", InPath,
                          PlaceholderPolicy::TemplateField,
                          OutIncident.incidentDate, OutError) ||
      !ReadRequiredString(InValue, "root_cause", InPath,
                          PlaceholderPolicy::ExactSentinel,
                          OutIncident.rootCause, OutError) ||
      !ReadRequiredString(InValue, "backlog_ref", InPath,
                          PlaceholderPolicy::TemplateField,
                          OutIncident.backlogRef, OutError) ||
      !ReadRequiredString(InValue, "change_ref", InPath,
                          PlaceholderPolicy::TemplateField,
                          OutIncident.changeRef, OutError) ||
      !ReadUniqueStringArray(InValue, "workflow_contracts", InPath,
                             OutIncident.workflowContracts, OutError)) {
    return false;
  }

  static const std::regex incidentIdPattern(R"(^[A-Z][A-Z0-9-]*-[0-9]{4}$)");
  static const std::regex datePattern(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}$)");
  if (!std::regex_match(OutIncident.incidentId, incidentIdPattern)) {
    return SetError(OutError, InPath + ".incident_id",
                    "expected stable item id ending in four digits");
  }
  if (OutIncident.incidentId.ends_with("-0000")) {
    return SetError(OutError, InPath + ".incident_id",
                    "template sentinel ids are not allowed");
  }
  if (!std::regex_match(OutIncident.incidentDate, datePattern)) {
    return SetError(OutError, InPath + ".incident_date", "expected YYYY-MM-DD");
  }
  const int year = std::stoi(OutIncident.incidentDate.substr(0, 4));
  const int month = std::stoi(OutIncident.incidentDate.substr(5, 2));
  const int day = std::stoi(OutIncident.incidentDate.substr(8, 2));
  static constexpr int daysPerMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
  };
  if (month < 1 || month > 12) {
    return SetError(OutError, InPath + ".incident_date",
                    "expected valid calendar date");
  }
  int maximumDay = daysPerMonth[month];
  const bool leapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leapYear) {
    maximumDay = 29;
  }
  if (day < 1 || day > maximumDay) {
    return SetError(OutError, InPath + ".incident_date",
                    "expected valid calendar date");
  }
  if (OutIncident.backlogRef != OutIncident.incidentId) {
    return SetError(OutError, InPath + ".backlog_ref",
                    "must equal incident_id");
  }
  if (!InOutIncidentIds.insert(OutIncident.incidentId).second) {
    return SetError(OutError, InPath + ".incident_id",
                    "duplicate incident id '" + OutIncident.incidentId + "'");
  }

  const auto &cases = InValue.at("regression_cases");
  if (!cases.is_array()) {
    return SetError(OutError, InPath + ".regression_cases", "expected array");
  }
  for (std::size_t index = 0; index < cases.size(); ++index) {
    RegressionCase parsedCase;
    if (!ParseRegressionCase(cases[index], OutIncident.incidentId,
                             InPath + ".regression_cases[" +
                                 std::to_string(index) + "]",
                             InOutCaseIds, parsedCase, OutError)) {
      return false;
    }
    OutIncident.regressionCases.push_back(std::move(parsedCase));
  }
  std::sort(OutIncident.regressionCases.begin(),
            OutIncident.regressionCases.end(),
            [](const RegressionCase &InLeft, const RegressionCase &InRight) {
              return InLeft.caseId < InRight.caseId;
            });
  return true;
}

auto CaseToJson(const RegressionCase &InCase) -> Json {
  return Json{
      {"case_id", InCase.caseId},     {"mapping_state", InCase.mappingState},
      {"test_file", InCase.testFile}, {"test_kind", InCase.testKind},
      {"test_name", InCase.testName},
  };
}

auto IncidentToJson(const Incident &InIncident) -> Json {
  Json cases = Json::array();
  for (const auto &regressionCase : InIncident.regressionCases) {
    cases.push_back(CaseToJson(regressionCase));
  }
  return Json{
      {"backlog_ref", InIncident.backlogRef},
      {"change_ref", InIncident.changeRef},
      {"incident_date", InIncident.incidentDate},
      {"incident_id", InIncident.incidentId},
      {"regression_cases", std::move(cases)},
      {"root_cause", InIncident.rootCause},
      {"workflow_contracts", InIncident.workflowContracts},
  };
}

auto LinkedCaseCount(const CoverageReport &InReport) -> std::size_t {
  std::size_t count = 0;
  for (const auto &incident : InReport.incidents) {
    count += incident.regressionCases.size();
  }
  return count;
}

} // namespace

auto LoadCoverageManifest(const std::filesystem::path &InManifestPath)
    -> LoadResult {
  LoadResult result;
  result.report.manifestPath = InManifestPath.lexically_normal();

  std::ifstream input(InManifestPath, std::ios::in | std::ios::binary);
  if (!input) {
    result.error =
        "manifest: cannot open '" + InManifestPath.generic_string() + "'";
    return result;
  }

  Json document;
  try {
    document = Json::parse(input);
  } catch (const Json::parse_error &error) {
    result.error = "manifest: invalid JSON: " + std::string(error.what());
    return result;
  }

  static const std::set<std::string> rootKeys = {
      "incidents",
      "schema",
  };
  if (!HasExactKeys(document, rootKeys, "manifest", result.error)) {
    return result;
  }
  if (!document.at("schema").is_string() ||
      document.at("schema").get<std::string>() != kManifestSchema) {
    result.error =
        "manifest.schema: expected '" + std::string(kManifestSchema) + "'";
    return result;
  }
  result.report.manifestSchema = document.at("schema").get<std::string>();

  const auto &incidents = document.at("incidents");
  if (!incidents.is_array()) {
    result.error = "manifest.incidents: expected array";
    return result;
  }
  if (incidents.empty()) {
    result.error = "manifest.incidents: must contain at least one incident";
    return result;
  }

  std::set<std::string> incidentIds;
  std::set<std::string> caseIds;
  for (std::size_t index = 0; index < incidents.size(); ++index) {
    Incident incident;
    if (!ParseIncident(incidents[index],
                       "manifest.incidents[" + std::to_string(index) + "]",
                       incidentIds, caseIds, incident, result.error)) {
      return result;
    }
    if (incident.regressionCases.empty()) {
      result.report.gaps.push_back(incident.incidentId);
    }
    result.report.incidents.push_back(std::move(incident));
  }

  std::sort(result.report.incidents.begin(), result.report.incidents.end(),
            [](const Incident &InLeft, const Incident &InRight) {
              if (InLeft.incidentDate != InRight.incidentDate) {
                return InLeft.incidentDate > InRight.incidentDate;
              }
              return InLeft.incidentId < InRight.incidentId;
            });
  std::sort(result.report.gaps.begin(), result.report.gaps.end());
  result.ok = true;
  return result;
}

auto RenderCoverageText(const CoverageReport &InReport) -> std::string {
  std::ostringstream output;
  output << "KOG regression coverage\n"
         << "manifest=" << InReport.manifestPath.generic_string() << "\n"
         << "manifest_schema=" << InReport.manifestSchema << "\n"
         << "execution_evidence=" << kExecutionEvidence << "\n"
         << "incidents=" << InReport.incidents.size() << "\n"
         << "linked_cases=" << LinkedCaseCount(InReport) << "\n"
         << "gaps=" << InReport.gaps.size() << "\n";

  for (const auto &incident : InReport.incidents) {
    output << "[" << (incident.regressionCases.empty() ? "gap" : "covered")
           << "] " << incident.incidentDate << " " << incident.incidentId
           << "\n"
           << "  root_cause=" << incident.rootCause << "\n"
           << "  backlog_ref=" << incident.backlogRef << "\n"
           << "  change_ref=" << incident.changeRef << "\n";
    for (const auto &workflowContract : incident.workflowContracts) {
      output << "  workflow_contract=" << workflowContract << "\n";
    }
    for (const auto &regressionCase : incident.regressionCases) {
      output << "  case=" << regressionCase.caseId
             << " kind=" << regressionCase.testKind
             << " mapping=" << regressionCase.mappingState << "\n"
             << "    test_name=" << regressionCase.testName << "\n"
             << "    test_file=" << regressionCase.testFile << "\n";
    }
  }
  return output.str();
}

auto RenderCoverageJson(const CoverageReport &InReport) -> std::string {
  Json incidents = Json::array();
  for (const auto &incident : InReport.incidents) {
    incidents.push_back(IncidentToJson(incident));
  }

  const Json output = {
      {"execution_evidence", kExecutionEvidence},
      {"gaps", InReport.gaps},
      {"incidents", std::move(incidents)},
      {"manifest", InReport.manifestPath.generic_string()},
      {"manifest_schema", InReport.manifestSchema},
      {"schema", kReportSchema},
      {"summary",
       {
           {"gaps", InReport.gaps.size()},
           {"incidents", InReport.incidents.size()},
           {"linked_cases", LinkedCaseCount(InReport)},
       }},
  };
  return output.dump(2) + "\n";
}

auto RenderCoverageErrorJson(const std::filesystem::path &InManifestPath,
                             const std::string &InError) -> std::string {
  const Json output = {
      {"error", InError},
      {"exit_code", kInvalidManifestExitCode},
      {"manifest", InManifestPath.lexically_normal().generic_string()},
      {"schema", kReportSchema},
      {"status", "invalid-manifest"},
  };
  return output.dump(2) + "\n";
}

auto CoverageExitCode(const CoverageReport &InReport, bool InFailOnGap) -> int {
  return InFailOnGap && !InReport.gaps.empty() ? kCoverageGapExitCode : 0;
}

} // namespace kano::git::commands::regression
