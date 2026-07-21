#pragma once

#include <kano_timing.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace kano::git::timing {

inline auto IsDiagnosticsEnabled() -> bool {
    const char* raw = std::getenv("KOG_DEBUG");
    if (raw == nullptr) {
        return false;
    }
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char InChar) {
        return static_cast<char>(std::tolower(InChar));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

class ScopedTimingLog final {
public:
    explicit ScopedTimingLog(std::string_view InLabel) {
        if (IsDiagnosticsEnabled()) {
            log_.emplace(InLabel);
        }
    }

    ScopedTimingLog(const ScopedTimingLog&) = delete;
    auto operator=(const ScopedTimingLog&) -> ScopedTimingLog& = delete;
    ScopedTimingLog(ScopedTimingLog&&) = delete;
    auto operator=(ScopedTimingLog&&) -> ScopedTimingLog& = delete;

private:
    std::optional<kano::infra::timing::ScopedTimingLog> log_;
};

class ScopedTimingLogWithElapsed final {
public:
    ScopedTimingLogWithElapsed(std::string_view InLabel, double& OutElapsed)
        : outElapsed_(OutElapsed) {
        outElapsed_ = 0.0;
        if (IsDiagnosticsEnabled()) {
            log_.emplace(InLabel, outElapsed_);
        }
    }

    ~ScopedTimingLogWithElapsed() noexcept {
        if (!log_.has_value()) {
            outElapsed_ = started_.elapsed_ms();
        }
    }

    ScopedTimingLogWithElapsed(const ScopedTimingLogWithElapsed&) = delete;
    auto operator=(const ScopedTimingLogWithElapsed&) -> ScopedTimingLogWithElapsed& = delete;
    ScopedTimingLogWithElapsed(ScopedTimingLogWithElapsed&&) = delete;
    auto operator=(ScopedTimingLogWithElapsed&&) -> ScopedTimingLogWithElapsed& = delete;

private:
    double& outElapsed_;
    kano::infra::timing::TimingPoint started_;
    std::optional<kano::infra::timing::ScopedTimingLogWithElapsed> log_;
};

} // namespace kano::git::timing

#define KOG_TIMING_VARIABLE_INNER__(line) kog_timing_scope_##line
#define KOG_TIMING_VARIABLE__(line) KOG_TIMING_VARIABLE_INNER__(line)
#define KOG_SCOPED_TIMING_LOG(label) \
    kano::git::timing::ScopedTimingLog KOG_TIMING_VARIABLE__(__LINE__)(label)
#define KOG_SCOPED_TIMING_LOG_WITH_ELAPSED(label, out_ms_ref) \
    kano::git::timing::ScopedTimingLogWithElapsed KOG_TIMING_VARIABLE__(__LINE__)(label, out_ms_ref)
