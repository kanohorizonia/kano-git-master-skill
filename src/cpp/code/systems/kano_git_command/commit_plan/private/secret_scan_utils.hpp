#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace kano::git::commands::secret_scan {

inline auto ToLowerCopy(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

inline auto LooksLikeIntentionalPlaceholderValue(const std::string& InValue) -> bool {
    const auto value = ToLowerCopy(InValue);
    if (value.empty()) {
        return false;
    }

    if ((value.size() >= 2 && value.front() == '<' && value.back() == '>') ||
        (value.size() >= 3 && value.front() == '$' && value[1] == '{' && value.back() == '}')) {
        return true;
    }

    if (value.starts_with("your-") || value.starts_with("your_") ||
        value.starts_with("replace-") || value.starts_with("replace_")) {
        return true;
    }

    static constexpr std::string_view kPlaceholderMarkers[] = {
        "example",
        "sample",
        "placeholder",
        "dummy",
        "replace",
        "change-me",
        "changeme",
        "fake",
        "mock",
        "your-api-key-here",
        "your-token-here",
        "your-secret-here",
    };

    for (const auto marker : kPlaceholderMarkers) {
        if (value.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline auto LooksLikeDynamicSecretReferenceValue(const std::string& InValue) -> bool {
    if (InValue.empty()) {
        return false;
    }

    static const std::regex kDynamicReferencePattern(
        R"(^\s*(?:\\?\$\([^)]+\)|\\?\$\{[^}]+\}|\\?\$[A-Za-z_][A-Za-z0-9_]*|env\s*:\s*[A-Za-z_][A-Za-z0-9_]*)\s*$)",
        std::regex::ECMAScript);

    return std::regex_match(InValue, kDynamicReferencePattern);
}

inline auto LooksLikeLogicalSecretReferenceValue(const std::string& InValue) -> bool {
    if (InValue.empty() || (InValue.find('_') == std::string::npos && InValue.find('-') == std::string::npos)) {
        return false;
    }

    static const std::regex kLogicalReferencePattern(
        R"(^\s*[a-z][a-z0-9_-]*(?:\.[a-z][a-z0-9_-]*)+\s*$)",
        std::regex::ECMAScript);

    return std::regex_match(InValue, kLogicalReferencePattern);
}

inline auto ExtractQuotedSecretAssignmentValue(const std::string& InLine) -> std::optional<std::string> {
    static const std::regex kSecretAssignmentStartPattern(
        R"((password|passwd|pwd|secret|api[_ -]?key|token)\s*[:=]\s*(['"]))",
        std::regex::ECMAScript | std::regex::icase);

    std::smatch match;
    if (!std::regex_search(InLine, match, kSecretAssignmentStartPattern) || match.size() < 3) {
        return std::nullopt;
    }

    const char quote = match[2].str().front();
    const auto valueStart = static_cast<std::size_t>(match.position(2) + match.length(2));
    const auto valueEnd = InLine.find_last_of(quote);
    if (valueEnd == std::string::npos || valueEnd <= valueStart) {
        return std::nullopt;
    }
    return InLine.substr(valueStart, valueEnd - valueStart);
}

inline auto ShouldIgnoreSecretFinding(const std::string& InRuleId,
                                      const std::string& InLine) -> bool {
    const auto lowerLine = ToLowerCopy(InLine);
    if (lowerLine.find("gitleaks:allow") != std::string::npos ||
        lowerLine.find("kog-secret:allow") != std::string::npos) {
        return true;
    }

    if (InRuleId != "generic_password_assignment") {
        return false;
    }

    const auto assignedValue = ExtractQuotedSecretAssignmentValue(InLine);
    if (!assignedValue.has_value() || assignedValue->size() < 8) {
        return false;
    }

    return LooksLikeIntentionalPlaceholderValue(*assignedValue)
        || LooksLikeDynamicSecretReferenceValue(*assignedValue)
        || LooksLikeLogicalSecretReferenceValue(*assignedValue);
}

} // namespace kano::git::commands::secret_scan
