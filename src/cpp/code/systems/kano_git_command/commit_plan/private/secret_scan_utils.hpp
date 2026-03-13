#pragma once

#include <algorithm>
#include <cctype>
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

inline auto ShouldIgnoreSecretFinding(const std::string& InRuleId,
                                      const std::string& InLine) -> bool {
    if (InRuleId != "generic_password_assignment") {
        return false;
    }

    static const std::regex kAssignmentPattern(
        R"(([Pp][Aa][Ss][Ss][Ww][Oo][Rr][Dd]|[Pp][Aa][Ss][Ss][Ww][Dd]|[Pp][Ww][Dd]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Aa][Pp][Ii][_ -]?[Kk][Ee][Yy]|[Tt][Oo][Kk][Ee][Nn])\s*[:=]\s*['\"]([^'\"]{8,})['\"])",
        std::regex::ECMAScript | std::regex::icase);

    std::smatch match;
    if (!std::regex_search(InLine, match, kAssignmentPattern) || match.size() < 3) {
        return false;
    }

    return LooksLikeIntentionalPlaceholderValue(match[2].str());
}

} // namespace kano::git::commands::secret_scan
