#include "audit_contract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace kano::git::audit {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMaxSafeJsonInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxDocumentBytes = 4U << 20U;
constexpr std::size_t kMaxJsonlBytes = 64U << 20U;
constexpr std::size_t kMaxStableIdBytes = 128;
constexpr std::size_t kMaxTokenBytes = 96;
constexpr std::size_t kMaxReferences = 32;
constexpr std::size_t kMaxArtifacts = 64;
constexpr std::size_t kMaxRepositories = 256;
constexpr std::size_t kMaxEvents = 1'000'000;
constexpr std::size_t kMaxValidationIssues = 1024;

void AddIssue(ValidationResult& Out, std::string InPath, std::string InCode,
              std::string InMessage) {
    if (Out.issues.size() >= kMaxValidationIssues) {
        return;
    }
    Out.issues.push_back(ValidationIssue{std::move(InPath), std::move(InCode),
                                         std::move(InMessage)});
}

auto JoinValidationPath(std::string_view InPrefix, std::string_view InPath)
    -> std::string {
    if (InPrefix.empty()) {
        return std::string(InPath);
    }
    if (InPath == "$") {
        return std::string(InPrefix);
    }
    if (InPath.size() > 1 && InPath.front() == '$' &&
        (InPath[1] == '.' || InPath[1] == '[')) {
        return std::string(InPrefix) + std::string(InPath.substr(1));
    }
    return std::string(InPrefix) + std::string(InPath);
}

void AppendIssues(ValidationResult& Out, const ValidationResult& InOther,
                  std::string_view InPathPrefix = {}) {
    for (const auto& issue : InOther.issues) {
        auto path = JoinValidationPath(InPathPrefix, issue.path);
        AddIssue(Out, std::move(path), issue.code, issue.message);
    }
}

auto IsLowerHex(std::string_view InValue, std::size_t InLength) -> bool {
    if (InValue.size() != InLength) {
        return false;
    }
    return std::all_of(InValue.begin(), InValue.end(), [](const char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

auto IsSha256(std::string_view InValue) -> bool {
    return IsLowerHex(InValue, 64);
}

auto IsGitObjectId(std::string_view InValue) -> bool {
    return IsLowerHex(InValue, 40) || IsLowerHex(InValue, 64);
}

auto RotateRight32(std::uint32_t InValue, int InBits) -> std::uint32_t {
    return (InValue >> InBits) | (InValue << (32 - InBits));
}

void ProcessSha256Block(const unsigned char* InBlock,
                        std::array<std::uint32_t, 8>& InOutState) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        words[index] = (static_cast<std::uint32_t>(InBlock[offset]) << 24) |
            (static_cast<std::uint32_t>(InBlock[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(InBlock[offset + 2]) << 8) |
            static_cast<std::uint32_t>(InBlock[offset + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const auto small0 = RotateRight32(words[index - 15], 7) ^
            RotateRight32(words[index - 15], 18) ^
            (words[index - 15] >> 3);
        const auto small1 = RotateRight32(words[index - 2], 17) ^
            RotateRight32(words[index - 2], 19) ^
            (words[index - 2] >> 10);
        words[index] = words[index - 16] + small0 + words[index - 7] + small1;
    }

    auto a = InOutState[0];
    auto b = InOutState[1];
    auto c = InOutState[2];
    auto d = InOutState[3];
    auto e = InOutState[4];
    auto f = InOutState[5];
    auto g = InOutState[6];
    auto h = InOutState[7];
    for (std::size_t index = 0; index < 64; ++index) {
        const auto large1 =
            RotateRight32(e, 6) ^ RotateRight32(e, 11) ^ RotateRight32(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary1 =
            h + large1 + choose + constants[index] + words[index];
        const auto large0 =
            RotateRight32(a, 2) ^ RotateRight32(a, 13) ^ RotateRight32(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = large0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    InOutState[0] += a;
    InOutState[1] += b;
    InOutState[2] += c;
    InOutState[3] += d;
    InOutState[4] += e;
    InOutState[5] += f;
    InOutState[6] += g;
    InOutState[7] += h;
}

auto Sha256(std::string_view InValue) -> std::string {
    std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                       0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                       0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block{};
    std::size_t offset = 0;
    while (offset + block.size() <= InValue.size()) {
        ProcessSha256Block(
            reinterpret_cast<const unsigned char*>(InValue.data() + offset),
            state);
        offset += block.size();
    }

    const auto remainder = InValue.size() - offset;
    std::copy_n(reinterpret_cast<const unsigned char*>(InValue.data() + offset),
                remainder, block.begin());
    block[remainder] = 0x80U;
    if (remainder >= 56) {
        ProcessSha256Block(block.data(), state);
        block.fill(0);
    }
    const auto bitLength = static_cast<std::uint64_t>(InValue.size()) * 8ULL;
    for (std::size_t index = 0; index < 8; ++index) {
        block[56 + index] =
            static_cast<unsigned char>((bitLength >> ((7 - index) * 8)) & 0xffU);
    }
    ProcessSha256Block(block.data(), state);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto part : state) {
        output << std::setw(8) << part;
    }
    return output.str();
}

auto Utf8CodePointCount(std::string_view InValue)
    -> std::optional<std::size_t> {
    std::size_t count = 0;
    std::size_t cursor = 0;
    const auto continuation = [&InValue](std::size_t InIndex) {
        return InIndex < InValue.size() &&
            (static_cast<unsigned char>(InValue[InIndex]) & 0xC0U) == 0x80U;
    };
    while (cursor < InValue.size()) {
        const auto first = static_cast<unsigned char>(InValue[cursor]);
        if (first <= 0x7FU) {
            ++cursor;
        } else if (first >= 0xC2U && first <= 0xDFU && continuation(cursor + 1)) {
            cursor += 2;
        } else if (first == 0xE0U && cursor + 2 < InValue.size() &&
                   static_cast<unsigned char>(InValue[cursor + 1]) >= 0xA0U &&
                   static_cast<unsigned char>(InValue[cursor + 1]) <= 0xBFU &&
                   continuation(cursor + 2)) {
            cursor += 3;
        } else if (((first >= 0xE1U && first <= 0xECU) ||
                    (first >= 0xEEU && first <= 0xEFU)) &&
                   continuation(cursor + 1) && continuation(cursor + 2)) {
            cursor += 3;
        } else if (first == 0xEDU && cursor + 2 < InValue.size() &&
                   static_cast<unsigned char>(InValue[cursor + 1]) >= 0x80U &&
                   static_cast<unsigned char>(InValue[cursor + 1]) <= 0x9FU &&
                   continuation(cursor + 2)) {
            cursor += 3;
        } else if (first == 0xF0U && cursor + 3 < InValue.size() &&
                   static_cast<unsigned char>(InValue[cursor + 1]) >= 0x90U &&
                   static_cast<unsigned char>(InValue[cursor + 1]) <= 0xBFU &&
                   continuation(cursor + 2) && continuation(cursor + 3)) {
            cursor += 4;
        } else if (first >= 0xF1U && first <= 0xF3U && continuation(cursor + 1) &&
                   continuation(cursor + 2) && continuation(cursor + 3)) {
            cursor += 4;
        } else if (first == 0xF4U && cursor + 3 < InValue.size() &&
                   static_cast<unsigned char>(InValue[cursor + 1]) >= 0x80U &&
                   static_cast<unsigned char>(InValue[cursor + 1]) <= 0x8FU &&
                   continuation(cursor + 2) && continuation(cursor + 3)) {
            cursor += 4;
        } else {
            return std::nullopt;
        }
        ++count;
    }
    return count;
}

auto IsBoundedText(std::string_view InValue, std::size_t InMaximum,
                   bool InAllowWhitespace) -> bool {
    const auto codePoints = Utf8CodePointCount(InValue);
    if (!codePoints.has_value() || *codePoints == 0 || *codePoints > InMaximum) {
        return false;
    }
    return std::none_of(
        InValue.begin(), InValue.end(), [InAllowWhitespace](const char value) {
            const auto byte = static_cast<unsigned char>(value);
            if (byte < 0x20U || byte == 0x7FU) {
                return true;
            }
            return !InAllowWhitespace && byte < 0x80U && std::isspace(byte) != 0;
        });
}

auto IsPrintableOpaqueId(std::string_view InValue) -> bool {
    return !InValue.empty() && InValue.size() <= 256 &&
        std::all_of(InValue.begin(), InValue.end(), [](const char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

auto IsOpaqueId(std::string_view InValue) -> bool {
    return IsPrintableOpaqueId(InValue);
}

auto IsSemanticToken(std::string_view InValue) -> bool {
    if (InValue.empty() || InValue.size() > kMaxTokenBytes) {
        return false;
    }
    if (InValue.front() < 'a' || InValue.front() > 'z') {
        return false;
    }
    return std::all_of(InValue.begin(), InValue.end(), [](const char value) {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
            value == '.' || value == '_' || value == '-';
    });
}

auto IsLogicalRepositoryId(std::string_view InValue) -> bool {
    if (!IsPrintableOpaqueId(InValue) || InValue.front() == '/' ||
        InValue.front() == '\\' || InValue.find('\\') != std::string_view::npos) {
        return false;
    }
    if (InValue.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(InValue[0])) != 0 &&
        InValue[1] == ':') {
        return false;
    }

    std::size_t cursor = 0;
    while (cursor <= InValue.size()) {
        const auto separator = InValue.find('/', cursor);
        const auto end =
            separator == std::string_view::npos ? InValue.size() : separator;
        const auto segment = InValue.substr(cursor, end - cursor);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        cursor = separator + 1;
    }
    return true;
}

auto IsContentType(std::string_view InValue) -> bool {
    if (InValue.empty() || InValue.size() > 128 ||
        !std::all_of(InValue.begin(), InValue.end(), [](const char value) {
            const auto byte = static_cast<unsigned char>(value);
            return byte >= 0x21U && byte <= 0x7EU;
        })) {
        return false;
    }
    const auto slash = InValue.find('/');
    return slash != std::string_view::npos && slash != 0 &&
        slash + 1 < InValue.size() &&
        InValue.find('/', slash + 1) == std::string_view::npos;
}

auto ParseDigits(std::string_view InValue, std::size_t InOffset,
                 std::size_t InCount) -> int {
    int result = 0;
    for (std::size_t index = 0; index < InCount; ++index) {
        const char value = InValue[InOffset + index];
        if (value < '0' || value > '9') {
            return -1;
        }
        result = (result * 10) + (value - '0');
    }
    return result;
}

auto IsStrictUtcTimestamp(std::string_view InValue) -> bool {
    if (InValue.size() != 20 || InValue[4] != '-' || InValue[7] != '-' ||
        InValue[10] != 'T' || InValue[13] != ':' || InValue[16] != ':' ||
        InValue[19] != 'Z') {
        return false;
    }

    const int yearValue = ParseDigits(InValue, 0, 4);
    const int monthValue = ParseDigits(InValue, 5, 2);
    const int dayValue = ParseDigits(InValue, 8, 2);
    const int hourValue = ParseDigits(InValue, 11, 2);
    const int minuteValue = ParseDigits(InValue, 14, 2);
    const int secondValue = ParseDigits(InValue, 17, 2);
    if (yearValue < 1 || monthValue < 1 || dayValue < 1 || hourValue < 0 ||
        hourValue > 23 || minuteValue < 0 || minuteValue > 59 ||
        secondValue < 0 || secondValue > 59) {
        return false;
    }

    const std::chrono::year_month_day date{
        std::chrono::year{yearValue},
        std::chrono::month{static_cast<unsigned int>(monthValue)},
        std::chrono::day{static_cast<unsigned int>(dayValue)}};
    return date.ok();
}

auto NormalizedKey(std::string_view InKey) -> std::string {
    std::string result;
    result.reserve(InKey.size());
    for (const char value : InKey) {
        const auto byte = static_cast<unsigned char>(value);
        if (std::isalnum(byte) != 0) {
            result.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return result;
}

auto IsForbiddenField(std::string_view InKey) -> bool {
    static constexpr std::array<std::string_view, 29> forbidden{
        "apikey", "argv", "auth", "authorization",
        "body", "command", "credential", "credentials",
        "env", "environment", "error", "failed",
        "message", "ok", "password", "path",
        "payload", "privatekey", "rawcommand", "secret",
        "secrets", "stderr", "stdout", "succeeded",
        "success", "token", "tokens", "username",
        "value"};
    const auto normalized = NormalizedKey(InKey);
    return std::find(forbidden.begin(), forbidden.end(), normalized) !=
        forbidden.end();
}

void CheckObjectKeys(const Json& InObject,
                     std::initializer_list<std::string_view> InAllowed,
                     std::string_view InPath, bool InAllowAdditive,
                     ValidationResult& Out) {
    if (!InObject.is_object()) {
        AddIssue(Out, std::string(InPath), "expected_object",
                 "Expected a JSON object.");
        return;
    }

    for (const auto& [key, value] : InObject.items()) {
        (void)value;
        const bool allowed =
            std::find(InAllowed.begin(), InAllowed.end(), key) != InAllowed.end();
        if (allowed) {
            continue;
        }
        if (IsForbiddenField(key)) {
            AddIssue(Out, std::string(InPath) + "." + key, "forbidden_field",
                     "Raw, sensitive, or conflicting fields are not part of the "
                     "audit contract.");
        } else if (!InAllowAdditive) {
            AddIssue(Out, std::string(InPath) + "." + key, "unknown_field",
                     "The field is not defined by this versioned object.");
        }
    }
}

auto RequiredString(const Json& InObject, std::string_view InKey,
                    std::string_view InPath, ValidationResult& Out)
    -> std::string {
    const auto iterator = InObject.find(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, std::string(InPath) + "." + std::string(InKey),
                 "missing_required_field", "A required string field is missing.");
        return {};
    }
    if (!iterator->is_string()) {
        AddIssue(Out, std::string(InPath) + "." + std::string(InKey),
                 "expected_string", "Expected a JSON string.");
        return {};
    }
    return iterator->get<std::string>();
}

auto RequiredNullableString(const Json& InObject, std::string_view InKey,
                            std::string_view InPath, ValidationResult& Out)
    -> std::optional<std::string> {
    const auto iterator = InObject.find(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, std::string(InPath) + "." + std::string(InKey),
                 "missing_required_field",
                 "A required nullable string field is missing.");
        return std::nullopt;
    }
    if (iterator->is_null()) {
        return std::nullopt;
    }
    if (!iterator->is_string()) {
        AddIssue(Out, std::string(InPath) + "." + std::string(InKey),
                 "expected_nullable_string", "Expected a string or null.");
        return std::nullopt;
    }
    return iterator->get<std::string>();
}

auto RequiredUnsigned(const Json& InObject, std::string_view InKey,
                      std::string_view InPath, ValidationResult& Out)
    -> std::uint64_t {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required integer field is missing.");
        return 0;
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value > kMaxSafeJsonInteger) {
            AddIssue(Out, path, "integer_out_of_range",
                     "Integer exceeds the JSON safe range.");
            return 0;
        }
        return value;
    }
    if (iterator->is_number_integer()) {
        const auto value = iterator->get<std::int64_t>();
        if (value >= 0 &&
            static_cast<std::uint64_t>(value) <= kMaxSafeJsonInteger) {
            return static_cast<std::uint64_t>(value);
        }
    }
    AddIssue(Out, path, "expected_safe_unsigned",
             "Expected a non-negative JSON-safe integer.");
    return 0;
}

auto RequiredUint32(const Json& InObject, std::string_view InKey,
                    std::string_view InPath, ValidationResult& Out)
    -> std::uint32_t {
    const auto value = RequiredUnsigned(InObject, InKey, InPath, Out);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        AddIssue(Out, std::string(InPath) + "." + std::string(InKey),
                 "integer_out_of_range",
                 "Integer exceeds the supported 32-bit range.");
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

auto RequiredNullableUnsigned(const Json& InObject, std::string_view InKey,
                              std::string_view InPath, ValidationResult& Out)
    -> std::optional<std::uint64_t> {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required nullable integer field is missing.");
        return std::nullopt;
    }
    if (iterator->is_null()) {
        return std::nullopt;
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value <= kMaxSafeJsonInteger) {
            return value;
        }
    } else if (iterator->is_number_integer()) {
        const auto value = iterator->get<std::int64_t>();
        if (value >= 0 &&
            static_cast<std::uint64_t>(value) <= kMaxSafeJsonInteger) {
            return static_cast<std::uint64_t>(value);
        }
    }
    AddIssue(Out, path, "expected_nullable_safe_unsigned",
             "Expected null or a non-negative JSON-safe integer.");
    return std::nullopt;
}

auto RequiredNullableInt(const Json& InObject, std::string_view InKey,
                         std::string_view InPath, ValidationResult& Out)
    -> std::optional<int> {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required nullable integer field is missing.");
        return std::nullopt;
    }
    if (iterator->is_null()) {
        return std::nullopt;
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            AddIssue(Out, path, "integer_out_of_range",
                     "Integer exceeds the supported exit-code range.");
            return std::nullopt;
        }
        return static_cast<int>(value);
    }
    if (!iterator->is_number_integer()) {
        AddIssue(Out, path, "expected_nullable_integer",
                 "Expected an integer or null.");
        return std::nullopt;
    }
    const auto value = iterator->get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        AddIssue(Out, path, "integer_out_of_range",
                 "Integer exceeds the supported exit-code range.");
        return std::nullopt;
    }
    return static_cast<int>(value);
}

auto RequiredBool(const Json& InObject, std::string_view InKey,
                  std::string_view InPath, ValidationResult& Out) -> bool {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required boolean field is missing.");
        return false;
    }
    if (!iterator->is_boolean()) {
        AddIssue(Out, path, "expected_boolean", "Expected a JSON boolean.");
        return false;
    }
    return iterator->get<bool>();
}

auto RequiredObject(const Json& InObject, std::string_view InKey,
                    std::string_view InPath, ValidationResult& Out)
    -> const Json* {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required object field is missing.");
        return nullptr;
    }
    if (!iterator->is_object()) {
        AddIssue(Out, path, "expected_object", "Expected a JSON object.");
        return nullptr;
    }
    return &*iterator;
}

auto RequiredArray(const Json& InObject, std::string_view InKey,
                   std::string_view InPath, ValidationResult& Out)
    -> const Json* {
    const auto iterator = InObject.find(InKey);
    const auto path = std::string(InPath) + "." + std::string(InKey);
    if (iterator == InObject.end()) {
        AddIssue(Out, path, "missing_required_field",
                 "A required array field is missing.");
        return nullptr;
    }
    if (!iterator->is_array()) {
        AddIssue(Out, path, "expected_array", "Expected a JSON array.");
        return nullptr;
    }
    return &*iterator;
}

auto ParseOutcomeState(std::string_view InValue)
    -> std::optional<OutcomeState> {
    if (InValue == "succeeded") {
        return OutcomeState::Succeeded;
    }
    if (InValue == "failed") {
        return OutcomeState::Failed;
    }
    if (InValue == "partial") {
        return OutcomeState::Partial;
    }
    if (InValue == "blocked") {
        return OutcomeState::Blocked;
    }
    if (InValue == "cancelled") {
        return OutcomeState::Cancelled;
    }
    if (InValue == "timed-out") {
        return OutcomeState::TimedOut;
    }
    if (InValue == "unknown") {
        return OutcomeState::Unknown;
    }
    return std::nullopt;
}

auto ParseRedactionStatus(std::string_view InValue)
    -> std::optional<RedactionStatus> {
    if (InValue == "not-required") {
        return RedactionStatus::NotRequired;
    }
    if (InValue == "redacted") {
        return RedactionStatus::Redacted;
    }
    if (InValue == "withheld") {
        return RedactionStatus::Withheld;
    }
    return std::nullopt;
}

auto ParseWorktreeState(std::string_view InValue)
    -> std::optional<WorktreeState> {
    if (InValue == "clean") {
        return WorktreeState::Clean;
    }
    if (InValue == "dirty") {
        return WorktreeState::Dirty;
    }
    if (InValue == "unknown") {
        return WorktreeState::Unknown;
    }
    return std::nullopt;
}

auto ParseCorrelationMode(std::string_view InValue)
    -> std::optional<CorrelationMode> {
    if (InValue == "standalone") {
        return CorrelationMode::Standalone;
    }
    if (InValue == "koa") {
        return CorrelationMode::Koa;
    }
    return std::nullopt;
}

auto IsKnownOutcomeState(OutcomeState InState) -> bool {
    switch (InState) {
    case OutcomeState::Succeeded:
    case OutcomeState::Failed:
    case OutcomeState::Partial:
    case OutcomeState::Blocked:
    case OutcomeState::Cancelled:
    case OutcomeState::TimedOut:
    case OutcomeState::Unknown:
        return true;
    }
    return false;
}

auto IsKnownRedactionStatus(RedactionStatus InStatus) -> bool {
    switch (InStatus) {
    case RedactionStatus::NotRequired:
    case RedactionStatus::Redacted:
    case RedactionStatus::Withheld:
        return true;
    }
    return false;
}

auto IsKnownWorktreeState(WorktreeState InState) -> bool {
    switch (InState) {
    case WorktreeState::Clean:
    case WorktreeState::Dirty:
    case WorktreeState::Unknown:
        return true;
    }
    return false;
}

auto IsKnownCorrelationMode(CorrelationMode InMode) -> bool {
    switch (InMode) {
    case CorrelationMode::Standalone:
    case CorrelationMode::Koa:
        return true;
    }
    return false;
}

template <typename TValue>
auto NullableJson(const std::optional<TValue>& InValue) -> Json {
    if (InValue.has_value()) {
        return Json(*InValue);
    }
    return Json(nullptr);
}

void ValidateTimestampRange(std::string_view InStarted,
                            std::string_view InFinished,
                            std::string_view InPath, ValidationResult& Out) {
    if (!IsStrictUtcTimestamp(InStarted)) {
        AddIssue(
            Out, std::string(InPath) + ".startedAtUtc", "invalid_utc_timestamp",
            "Expected a real-calendar UTC timestamp in YYYY-MM-DDTHH:MM:SSZ form.");
    }
    if (!IsStrictUtcTimestamp(InFinished)) {
        AddIssue(
            Out, std::string(InPath) + ".finishedAtUtc", "invalid_utc_timestamp",
            "Expected a real-calendar UTC timestamp in YYYY-MM-DDTHH:MM:SSZ form.");
    }
    if (IsStrictUtcTimestamp(InStarted) && IsStrictUtcTimestamp(InFinished) &&
        InFinished < InStarted) {
        AddIssue(Out, std::string(InPath) + ".finishedAtUtc", "finish_before_start",
                 "The finish timestamp cannot precede the start timestamp.");
    }
}

void ValidateOutcome(const Outcome& InOutcome, std::string_view InPath,
                     ValidationResult& Out) {
    if (!IsKnownOutcomeState(InOutcome.status)) {
        AddIssue(Out, std::string(InPath) + ".status", "invalid_outcome",
                 "Outcome is not one of the seven v1 terminal states.");
        return;
    }
    const bool succeeded = InOutcome.status == OutcomeState::Succeeded;
    if (succeeded) {
        if (InOutcome.reasonCode.has_value()) {
            AddIssue(Out, std::string(InPath) + ".reasonCode",
                     "contradictory_success",
                     "A succeeded outcome cannot carry a failure reason.");
        }
        if (InOutcome.exitCode.has_value() && *InOutcome.exitCode != 0) {
            AddIssue(Out, std::string(InPath) + ".exitCode", "contradictory_success",
                     "A succeeded outcome cannot carry a non-zero exit code.");
        }
        if (InOutcome.retryable) {
            AddIssue(Out, std::string(InPath) + ".retryable", "contradictory_success",
                     "A succeeded outcome cannot be retryable.");
        }
        return;
    }

    if (!InOutcome.reasonCode.has_value() || !IsOpaqueId(*InOutcome.reasonCode)) {
        AddIssue(
            Out, std::string(InPath) + ".reasonCode", "missing_terminal_reason",
            "Every non-success terminal outcome requires a bounded reason code.");
    }
    if (InOutcome.exitCode.has_value() && *InOutcome.exitCode == 0) {
        AddIssue(Out, std::string(InPath) + ".exitCode", "contradictory_failure",
                 "A non-success outcome cannot carry exit code zero.");
    }
}

void ValidateCorrelation(const CorrelationRefs& InCorrelation,
                         std::string_view InPath, ValidationResult& Out) {
    const std::array<
        std::pair<std::string_view, const std::optional<std::string>*>, 8>
        fields{{
            {"productId", &InCorrelation.productId},
            {"topicId", &InCorrelation.topicId},
            {"itemId", &InCorrelation.itemId},
            {"workOrderId", &InCorrelation.workOrderId},
            {"requestId", &InCorrelation.requestId},
            {"producerId", &InCorrelation.producerId},
            {"routeId", &InCorrelation.routeId},
            {"agentId", &InCorrelation.agentId},
        }};

    if (!IsKnownCorrelationMode(InCorrelation.mode)) {
        AddIssue(Out, std::string(InPath) + ".mode", "invalid_correlation_mode",
                 "Correlation mode must be standalone or koa.");
        return;
    }

    if (InCorrelation.mode == CorrelationMode::Standalone) {
        for (const auto& [name, value] : fields) {
            if (value->has_value()) {
                AddIssue(Out, std::string(InPath) + "." + std::string(name),
                         "standalone_correlation_conflict",
                         "Standalone mode must not claim KOA correlation identifiers.");
            }
        }
        return;
    }

    static constexpr std::array<std::string_view, 6> required{
        "productId", "itemId", "workOrderId",
        "requestId", "producerId", "routeId"};
    for (const auto& [name, value] : fields) {
        if (value->has_value() && !IsStableAuditId(**value)) {
            AddIssue(Out, std::string(InPath) + "." + std::string(name),
                     "invalid_correlation_id",
                     "Correlation identifiers must use the stable-ID grammar.");
        }
        if (std::find(required.begin(), required.end(), name) != required.end() &&
            !value->has_value()) {
            AddIssue(Out, std::string(InPath) + "." + std::string(name),
                     "incomplete_koa_correlation",
                     "KOA mode requires the allowlisted identity chain.");
        }
    }
}

void ValidateRepositoryState(const RepositoryState& InState,
                             std::string_view InPath, ValidationResult& Out) {
    if (InState.headSha.has_value() && !IsGitObjectId(*InState.headSha)) {
        AddIssue(
            Out, std::string(InPath) + ".headSha", "invalid_git_oid",
            "Git object IDs must be 40 or 64 lowercase hexadecimal characters.");
    }
    if (InState.branch.has_value() &&
        !IsBoundedText(*InState.branch, 512, true)) {
        AddIssue(Out, std::string(InPath) + ".branch", "invalid_branch",
                 "Branch must be null or bounded control-free text.");
    }

    if (!IsKnownWorktreeState(InState.worktreeState)) {
        AddIssue(Out, std::string(InPath) + ".worktreeState",
                 "invalid_worktree_state",
                 "Worktree state must be clean, dirty, or unknown.");
    } else if (InState.worktreeState == WorktreeState::Unknown) {
        if (InState.dirtyFingerprint.has_value()) {
            AddIssue(Out, std::string(InPath) + ".dirtyFingerprint",
                     "unknown_worktree_conflict",
                     "Unknown worktree state cannot claim a dirty fingerprint.");
        }
    } else if (!InState.dirtyFingerprint.has_value() ||
               !IsSha256(*InState.dirtyFingerprint)) {
        AddIssue(Out, std::string(InPath) + ".dirtyFingerprint",
                 "invalid_dirty_fingerprint",
                 "Known worktree state requires a lowercase SHA-256 fingerprint.");
    }

    const bool hasUpstream = InState.upstreamHeadSha.has_value();
    const bool hasAhead = InState.ahead.has_value();
    const bool hasBehind = InState.behind.has_value();
    if (!(hasUpstream == hasAhead && hasAhead == hasBehind)) {
        AddIssue(
            Out, std::string(InPath), "partial_upstream_state",
            "upstreamHeadSha, ahead, and behind must be all present or all null.");
    }
    if (hasUpstream && !IsGitObjectId(*InState.upstreamHeadSha)) {
        AddIssue(Out, std::string(InPath) + ".upstreamHeadSha", "invalid_git_oid",
                 "Upstream Git object IDs must be lowercase SHA-1 or SHA-256.");
    }
    if ((hasAhead && *InState.ahead > kMaxSafeJsonInteger) ||
        (hasBehind && *InState.behind > kMaxSafeJsonInteger)) {
        AddIssue(
            Out, std::string(InPath), "integer_out_of_range",
            "Repository counters must stay within the JSON safe integer range.");
    }
}

void ValidateRepositoryTransition(const RepositoryTransition& InRepository,
                                  std::string_view InPath,
                                  ValidationResult& Out) {
    if (!IsLogicalRepositoryId(InRepository.repositoryId)) {
        AddIssue(Out, std::string(InPath) + ".id", "invalid_repository_id",
                 "Repository IDs must be bounded logical relative identifiers.");
    }
    ValidateRepositoryState(InRepository.before, std::string(InPath) + ".before",
                            Out);
    ValidateRepositoryState(InRepository.after, std::string(InPath) + ".after",
                            Out);
}

void ValidateReferences(const std::vector<AuditReference>& InReferences,
                        std::string_view InPath, ValidationResult& Out) {
    if (InReferences.size() > kMaxReferences) {
        AddIssue(Out, std::string(InPath), "too_many_references",
                 "Reference arrays are bounded to 32 entries.");
    }
    std::set<std::string> ids;
    for (std::size_t index = 0; index < InReferences.size(); ++index) {
        const auto& reference = InReferences[index];
        const auto path = std::string(InPath) + "[" + std::to_string(index) + "]";
        if (!IsOpaqueId(reference.id)) {
            AddIssue(Out, path + ".id", "invalid_reference_id",
                     "Reference ID is invalid.");
        }
        if (!IsSha256(reference.sha256)) {
            AddIssue(Out, path + ".sha256", "invalid_sha256",
                     "Expected lowercase SHA-256.");
        }
        if (!ids.insert(reference.id).second) {
            AddIssue(Out, path + ".id", "duplicate_reference_id",
                     "Reference IDs must be unique.");
        }
    }
}

void ValidateArtifacts(const std::vector<ArtifactReference>& InArtifacts,
                       std::string_view InPath, ValidationResult& Out) {
    if (InArtifacts.size() > kMaxArtifacts) {
        AddIssue(Out, std::string(InPath), "too_many_artifacts",
                 "Artifact reference arrays are bounded to 64 entries.");
    }
    std::set<std::string> ids;
    for (std::size_t index = 0; index < InArtifacts.size(); ++index) {
        const auto& artifact = InArtifacts[index];
        const auto path = std::string(InPath) + "[" + std::to_string(index) + "]";
        if (!IsOpaqueId(artifact.id)) {
            AddIssue(Out, path + ".id", "invalid_artifact_id",
                     "Artifact ID is invalid.");
        }
        if (!IsSemanticToken(artifact.kind)) {
            AddIssue(Out, path + ".kind", "invalid_artifact_kind",
                     "Artifact kind is invalid.");
        }
        if (!IsSha256(artifact.sha256)) {
            AddIssue(Out, path + ".sha256", "invalid_sha256",
                     "Expected lowercase SHA-256.");
        }
        if (artifact.sizeBytes > kMaxSafeJsonInteger) {
            AddIssue(Out, path + ".sizeBytes", "integer_out_of_range",
                     "Artifact size exceeds the JSON safe range.");
        }
        if (!IsContentType(artifact.contentType)) {
            AddIssue(Out, path + ".contentType", "invalid_content_type",
                     "Artifact content type is invalid.");
        }
        if (!IsKnownRedactionStatus(artifact.redactionStatus)) {
            AddIssue(Out, path + ".redactionStatus", "invalid_redaction_status",
                     "Redaction status must be not-required, redacted, or withheld.");
        }
        if (!ids.insert(artifact.id).second) {
            AddIssue(Out, path + ".id", "duplicate_artifact_id",
                     "Artifact IDs must be unique.");
        }
    }
}

auto ToJson(const CorrelationRefs& InCorrelation) -> Json {
    return Json{
        {"mode", std::string(CorrelationModeName(InCorrelation.mode))},
        {"productId", NullableJson(InCorrelation.productId)},
        {"topicId", NullableJson(InCorrelation.topicId)},
        {"itemId", NullableJson(InCorrelation.itemId)},
        {"workOrderId", NullableJson(InCorrelation.workOrderId)},
        {"requestId", NullableJson(InCorrelation.requestId)},
        {"producerId", NullableJson(InCorrelation.producerId)},
        {"routeId", NullableJson(InCorrelation.routeId)},
        {"agentId", NullableJson(InCorrelation.agentId)},
    };
}

auto ToJson(const RepositoryState& InState) -> Json {
    return Json{
        {"headSha", NullableJson(InState.headSha)},
        {"branch", NullableJson(InState.branch)},
        {"worktreeState", std::string(WorktreeStateName(InState.worktreeState))},
        {"dirtyFingerprint", NullableJson(InState.dirtyFingerprint)},
        {"upstreamHeadSha", NullableJson(InState.upstreamHeadSha)},
        {"ahead", NullableJson(InState.ahead)},
        {"behind", NullableJson(InState.behind)},
    };
}

auto ToJson(const RepositoryTransition& InRepository) -> Json {
    return Json{
        {"id", InRepository.repositoryId},
        {"before", ToJson(InRepository.before)},
        {"after", ToJson(InRepository.after)},
    };
}

auto ToJson(const AuditReference& InReference) -> Json {
    return Json{{"id", InReference.id}, {"sha256", InReference.sha256}};
}

auto ToJson(const ArtifactReference& InArtifact) -> Json {
    return Json{
        {"id", InArtifact.id},
        {"kind", InArtifact.kind},
        {"sha256", InArtifact.sha256},
        {"sizeBytes", InArtifact.sizeBytes},
        {"contentType", InArtifact.contentType},
        {"redactionStatus",
         std::string(RedactionStatusName(InArtifact.redactionStatus))},
    };
}

auto ToJson(const Outcome& InOutcome) -> Json {
    return Json{
        {"status", std::string(OutcomeStateName(InOutcome.status))},
        {"exitCode", NullableJson(InOutcome.exitCode)},
        {"reasonCode", NullableJson(InOutcome.reasonCode)},
        {"retryable", InOutcome.retryable},
    };
}

auto ReferenceArray(const std::vector<AuditReference>& InReferences) -> Json {
    std::vector<AuditReference> sorted = InReferences;
    std::sort(
        sorted.begin(), sorted.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    Json result = Json::array();
    for (const auto& reference : sorted) {
        result.push_back(ToJson(reference));
    }
    return result;
}

auto ArtifactArray(const std::vector<ArtifactReference>& InArtifacts) -> Json {
    std::vector<ArtifactReference> sorted = InArtifacts;
    std::sort(
        sorted.begin(), sorted.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    Json result = Json::array();
    for (const auto& artifact : sorted) {
        result.push_back(ToJson(artifact));
    }
    return result;
}

auto RepositoryArray(const std::vector<RepositoryTransition>& InRepositories)
    -> Json {
    std::vector<RepositoryTransition> sorted = InRepositories;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) {
                  return left.repositoryId < right.repositoryId;
              });
    Json result = Json::array();
    for (const auto& repository : sorted) {
        result.push_back(ToJson(repository));
    }
    return result;
}

auto ToJson(const AuditEvent& InEvent) -> Json {
    return Json{
        {"schemaName", InEvent.schemaName},
        {"schemaVersion", InEvent.schemaVersion},
        {"eventId", InEvent.eventId},
        {"runId", InEvent.runId},
        {"parentRunId", NullableJson(InEvent.parentRunId)},
        {"attempt", InEvent.attempt},
        {"planId", InEvent.planId},
        {"planSha256", InEvent.planSha256},
        {"sequence", InEvent.sequence},
        {"startedAtUtc", InEvent.startedAtUtc},
        {"finishedAtUtc", InEvent.finishedAtUtc},
        {"repository", ToJson(InEvent.repository)},
        {"phase", InEvent.phase},
        {"action", InEvent.action},
        {"outcome", ToJson(InEvent.outcome)},
        {"correlation", ToJson(InEvent.correlation)},
        {"policyRefs", ReferenceArray(InEvent.policyRefs)},
        {"approvalRefs", ReferenceArray(InEvent.approvalRefs)},
        {"artifacts", ArtifactArray(InEvent.artifacts)},
    };
}

auto ToJson(const RunReceipt& InReceipt) -> Json {
    return Json{
        {"schemaName", InReceipt.schemaName},
        {"schemaVersion", InReceipt.schemaVersion},
        {"runId", InReceipt.runId},
        {"parentRunId", NullableJson(InReceipt.parentRunId)},
        {"attempt", InReceipt.attempt},
        {"planId", InReceipt.planId},
        {"planSha256", InReceipt.planSha256},
        {"startedAtUtc", InReceipt.startedAtUtc},
        {"finishedAtUtc", InReceipt.finishedAtUtc},
        {"firstSequence", InReceipt.firstSequence},
        {"lastSequence", InReceipt.lastSequence},
        {"eventCount", InReceipt.eventCount},
        {"eventStreamSha256", InReceipt.eventStreamSha256},
        {"terminalOutcome", ToJson(InReceipt.terminalOutcome)},
        {"correlation", ToJson(InReceipt.correlation)},
        {"repositories", RepositoryArray(InReceipt.repositories)},
        {"policyRefs", ReferenceArray(InReceipt.policyRefs)},
        {"approvalRefs", ReferenceArray(InReceipt.approvalRefs)},
        {"artifacts", ArtifactArray(InReceipt.artifacts)},
    };
}

auto ParseOutcome(const Json& InObject, std::string_view InPath,
                  ValidationResult& Out) -> Outcome {
    CheckObjectKeys(InObject, {"status", "exitCode", "reasonCode", "retryable"},
                    InPath, false, Out);
    Outcome result;
    const auto status = RequiredString(InObject, "status", InPath, Out);
    if (const auto parsed = ParseOutcomeState(status); parsed.has_value()) {
        result.status = *parsed;
    } else {
        AddIssue(Out, std::string(InPath) + ".status", "invalid_outcome",
                 "Outcome is not one of the seven v1 terminal states.");
    }
    result.exitCode = RequiredNullableInt(InObject, "exitCode", InPath, Out);
    result.reasonCode =
        RequiredNullableString(InObject, "reasonCode", InPath, Out);
    result.retryable = RequiredBool(InObject, "retryable", InPath, Out);
    return result;
}

auto ParseCorrelation(const Json& InObject, std::string_view InPath,
                      ValidationResult& Out) -> CorrelationRefs {
    CheckObjectKeys(InObject,
                    {"mode", "productId", "topicId", "itemId", "workOrderId",
                     "requestId", "producerId", "routeId", "agentId"},
                    InPath, false, Out);
    CorrelationRefs result;
    const auto mode = RequiredString(InObject, "mode", InPath, Out);
    if (const auto parsed = ParseCorrelationMode(mode); parsed.has_value()) {
        result.mode = *parsed;
    } else {
        AddIssue(Out, std::string(InPath) + ".mode", "invalid_correlation_mode",
                 "Correlation mode must be standalone or koa.");
    }
    result.productId = RequiredNullableString(InObject, "productId", InPath, Out);
    result.topicId = RequiredNullableString(InObject, "topicId", InPath, Out);
    result.itemId = RequiredNullableString(InObject, "itemId", InPath, Out);
    result.workOrderId =
        RequiredNullableString(InObject, "workOrderId", InPath, Out);
    result.requestId = RequiredNullableString(InObject, "requestId", InPath, Out);
    result.producerId =
        RequiredNullableString(InObject, "producerId", InPath, Out);
    result.routeId = RequiredNullableString(InObject, "routeId", InPath, Out);
    result.agentId = RequiredNullableString(InObject, "agentId", InPath, Out);
    return result;
}

auto ParseRepositoryState(const Json& InObject, std::string_view InPath,
                          ValidationResult& Out) -> RepositoryState {
    CheckObjectKeys(InObject,
                    {"headSha", "branch", "worktreeState", "dirtyFingerprint",
                     "upstreamHeadSha", "ahead", "behind"},
                    InPath, false, Out);
    RepositoryState result;
    result.headSha = RequiredNullableString(InObject, "headSha", InPath, Out);
    result.branch = RequiredNullableString(InObject, "branch", InPath, Out);
    const auto worktree = RequiredString(InObject, "worktreeState", InPath, Out);
    if (const auto parsed = ParseWorktreeState(worktree); parsed.has_value()) {
        result.worktreeState = *parsed;
    } else {
        AddIssue(Out, std::string(InPath) + ".worktreeState",
                 "invalid_worktree_state",
                 "Worktree state must be clean, dirty, or unknown.");
    }
    result.dirtyFingerprint =
        RequiredNullableString(InObject, "dirtyFingerprint", InPath, Out);
    result.upstreamHeadSha =
        RequiredNullableString(InObject, "upstreamHeadSha", InPath, Out);
    result.ahead = RequiredNullableUnsigned(InObject, "ahead", InPath, Out);
    result.behind = RequiredNullableUnsigned(InObject, "behind", InPath, Out);
    return result;
}

auto ParseRepositoryTransition(const Json& InObject, std::string_view InPath,
                               ValidationResult& Out) -> RepositoryTransition {
    CheckObjectKeys(InObject, {"id", "before", "after"}, InPath, false, Out);
    RepositoryTransition result;
    result.repositoryId = RequiredString(InObject, "id", InPath, Out);
    if (const auto* before = RequiredObject(InObject, "before", InPath, Out)) {
        result.before =
            ParseRepositoryState(*before, std::string(InPath) + ".before", Out);
    }
    if (const auto* after = RequiredObject(InObject, "after", InPath, Out)) {
        result.after =
            ParseRepositoryState(*after, std::string(InPath) + ".after", Out);
    }
    return result;
}

auto ParseReferences(const Json& InArray, std::string_view InPath,
                     ValidationResult& Out) -> std::vector<AuditReference> {
    std::vector<AuditReference> result;
    if (InArray.size() > kMaxReferences) {
        AddIssue(Out, std::string(InPath), "too_many_references",
                 "Reference arrays are bounded to 32 entries.");
        return result;
    }
    result.reserve(InArray.size());
    for (std::size_t index = 0; index < InArray.size(); ++index) {
        const auto path = std::string(InPath) + "[" + std::to_string(index) + "]";
        if (!InArray[index].is_object()) {
            AddIssue(Out, path, "expected_object", "Expected a reference object.");
            continue;
        }
        CheckObjectKeys(InArray[index], {"id", "sha256"}, path, false, Out);
        result.push_back(
            AuditReference{RequiredString(InArray[index], "id", path, Out),
                           RequiredString(InArray[index], "sha256", path, Out)});
    }
    return result;
}

auto ParseArtifacts(const Json& InArray, std::string_view InPath,
                    ValidationResult& Out) -> std::vector<ArtifactReference> {
    std::vector<ArtifactReference> result;
    if (InArray.size() > kMaxArtifacts) {
        AddIssue(Out, std::string(InPath), "too_many_artifacts",
                 "Artifact reference arrays are bounded to 64 entries.");
        return result;
    }
    result.reserve(InArray.size());
    for (std::size_t index = 0; index < InArray.size(); ++index) {
        const auto path = std::string(InPath) + "[" + std::to_string(index) + "]";
        if (!InArray[index].is_object()) {
            AddIssue(Out, path, "expected_object",
                     "Expected an artifact reference object.");
            continue;
        }
        const auto& object = InArray[index];
        CheckObjectKeys(
            object,
            {"id", "kind", "sha256", "sizeBytes", "contentType", "redactionStatus"},
            path, false, Out);
        ArtifactReference artifact;
        artifact.id = RequiredString(object, "id", path, Out);
        artifact.kind = RequiredString(object, "kind", path, Out);
        artifact.sha256 = RequiredString(object, "sha256", path, Out);
        artifact.sizeBytes = RequiredUnsigned(object, "sizeBytes", path, Out);
        artifact.contentType = RequiredString(object, "contentType", path, Out);
        const auto redaction = RequiredString(object, "redactionStatus", path, Out);
        if (const auto parsed = ParseRedactionStatus(redaction);
            parsed.has_value()) {
            artifact.redactionStatus = *parsed;
        } else {
            AddIssue(Out, path + ".redactionStatus", "invalid_redaction_status",
                     "Redaction status must be not-required, redacted, or withheld.");
        }
        result.push_back(std::move(artifact));
    }
    return result;
}

auto ParseDocument(std::string_view InJson, std::string_view InContract)
    -> std::pair<std::optional<Json>, ValidationResult> {
    ValidationResult validation;
    if (InJson.empty() || InJson.size() > kMaxDocumentBytes) {
        AddIssue(validation, "$", "invalid_document_size",
                 std::string(InContract) +
                     " must be non-empty and no larger than 4 MiB.");
        return {std::nullopt, std::move(validation)};
    }
    if (InJson.size() >= 3 && static_cast<unsigned char>(InJson[0]) == 0xEFU &&
        static_cast<unsigned char>(InJson[1]) == 0xBBU &&
        static_cast<unsigned char>(InJson[2]) == 0xBFU) {
        AddIssue(validation, "$", "bom_not_allowed", "UTF-8 BOM is not allowed.");
        return {std::nullopt, std::move(validation)};
    }
    try {
        bool duplicateKey = false;
        std::map<int, std::set<std::string>> objectKeys;
        const Json::parser_callback_t callback = [&duplicateKey, &objectKeys](
                                                     int InDepth,
                                                     Json::parse_event_t InEvent,
                                                     Json& InParsed) {
            if (InEvent == Json::parse_event_t::object_start) {
                objectKeys[InDepth].clear();
            } else if (InEvent == Json::parse_event_t::key) {
                const auto objectDepth = std::max(0, InDepth - 1);
                if (!objectKeys[objectDepth]
                         .insert(InParsed.get<std::string>())
                         .second) {
                    duplicateKey = true;
                }
            } else if (InEvent == Json::parse_event_t::object_end) {
                objectKeys.erase(InDepth);
            }
            return true;
        };
        auto document =
            Json::parse(InJson.begin(), InJson.end(), callback, true, false);
        if (duplicateKey) {
            AddIssue(validation, "$", "duplicate_json_key",
                     "Duplicate JSON object keys are ambiguous and are not allowed.");
            return {std::nullopt, std::move(validation)};
        }
        if (!document.is_object()) {
            AddIssue(validation, "$", "expected_object",
                     "The audit document must be a JSON object.");
            return {std::nullopt, std::move(validation)};
        }
        return {std::move(document), std::move(validation)};
    } catch (const Json::parse_error&) {
        AddIssue(validation, "$", "invalid_json",
                 "The audit document is not valid JSON.");
        return {std::nullopt, std::move(validation)};
    }
}

auto ValidateEventStream(std::span<const AuditEvent> InEvents,
                         bool InValidateTypedEvents = true)
    -> ValidationResult {
    ValidationResult result;
    if (InEvents.empty()) {
        AddIssue(result, "$", "empty_event_stream",
                 "An audit JSONL stream must contain at least one event.");
        return result;
    }
    if (InEvents.size() > kMaxEvents) {
        AddIssue(result, "$", "too_many_events",
                 "An audit stream exceeds the v1 event bound.");
        return result;
    }

    const auto& first = InEvents.front();
    std::set<std::string> eventIds;
    for (std::size_t index = 0; index < InEvents.size(); ++index) {
        const auto& event = InEvents[index];
        if (InValidateTypedEvents) {
            AppendIssues(result, ValidateAuditEvent(event),
                         "$[" + std::to_string(index) + "]");
        }
        if (!eventIds.insert(event.eventId).second) {
            AddIssue(result, "$[" + std::to_string(index) + "].eventId",
                     "duplicate_event_id",
                     "Event IDs must be unique within one run.");
        }
        const auto expected = static_cast<std::uint64_t>(index + 1);
        if (event.sequence != expected) {
            AddIssue(result, "$[" + std::to_string(index) + "].sequence",
                     event.sequence <= static_cast<std::uint64_t>(index)
                         ? "duplicate_or_non_monotonic_sequence"
                         : "non_contiguous_sequence",
                     "Event sequence must be contiguous and strictly increasing from "
                     "one.");
        }
        if (event.runId != first.runId || event.parentRunId != first.parentRunId ||
            event.attempt != first.attempt || event.planId != first.planId ||
            event.planSha256 != first.planSha256 ||
            event.correlation != first.correlation) {
            AddIssue(result, "$[" + std::to_string(index) + "]",
                     "event_identity_drift",
                     "Run, parent, attempt, plan, and correlation identities must "
                     "remain invariant.");
        }
    }
    return result;
}

} // namespace

auto ValidationResult::ok() const noexcept -> bool { return issues.empty(); }

auto AuditEventParseResult::ok() const noexcept -> bool {
    return value.has_value() && validation.ok();
}

auto RunReceiptParseResult::ok() const noexcept -> bool {
    return value.has_value() && validation.ok();
}

auto AuditEventsParseResult::ok() const noexcept -> bool {
    return !values.empty() && validation.ok();
}

auto SerializationResult::ok() const noexcept -> bool {
    return !json.empty() && validation.ok();
}

auto OutcomeStateName(OutcomeState InState) -> std::string_view {
    switch (InState) {
    case OutcomeState::Succeeded:
        return "succeeded";
    case OutcomeState::Failed:
        return "failed";
    case OutcomeState::Partial:
        return "partial";
    case OutcomeState::Blocked:
        return "blocked";
    case OutcomeState::Cancelled:
        return "cancelled";
    case OutcomeState::TimedOut:
        return "timed-out";
    case OutcomeState::Unknown:
        return "unknown";
    }
    return "unknown";
}

auto RedactionStatusName(RedactionStatus InStatus) -> std::string_view {
    switch (InStatus) {
    case RedactionStatus::NotRequired:
        return "not-required";
    case RedactionStatus::Redacted:
        return "redacted";
    case RedactionStatus::Withheld:
        return "withheld";
    }
    return "withheld";
}

auto WorktreeStateName(WorktreeState InState) -> std::string_view {
    switch (InState) {
    case WorktreeState::Clean:
        return "clean";
    case WorktreeState::Dirty:
        return "dirty";
    case WorktreeState::Unknown:
        return "unknown";
    }
    return "unknown";
}

auto CorrelationModeName(CorrelationMode InMode) -> std::string_view {
    switch (InMode) {
    case CorrelationMode::Standalone:
        return "standalone";
    case CorrelationMode::Koa:
        return "koa";
    }
    return "standalone";
}

auto Sha256Hex(std::string_view InBytes) -> std::string {
    return Sha256(InBytes);
}

auto IsStableAuditId(const std::string_view InValue) -> bool {
    if (InValue.empty() || InValue.size() > kMaxStableIdBytes ||
        InValue.find("..") != std::string_view::npos) {
        return false;
    }
    const auto alphanumeric = [](const char value) {
        return (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9');
    };
    if (!alphanumeric(InValue.front())) return false;
    if (!std::all_of(InValue.begin(), InValue.end(), [&](const char value) {
            return alphanumeric(value) || value == '.' || value == '_' ||
                value == ':' || value == '@' || value == '-';
        })) {
        return false;
    }
    std::string lowered(InValue);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    static constexpr std::array<std::string_view, 6> secretPrefixes = {
        "sk-", "ghp_", "github_pat_", "glpat-", "bearer", "akia"};
    return std::none_of(secretPrefixes.begin(), secretPrefixes.end(), [&](const auto prefix) {
        return lowered.starts_with(prefix);
    });
}

auto ValidateAuditEvent(const AuditEvent& InEvent) -> ValidationResult {
    ValidationResult result;
    if (InEvent.schemaName != kAuditEventSchemaName ||
        InEvent.schemaVersion != kAuditSchemaVersionV1) {
        AddIssue(result, "$", "unsupported_schema",
                 "Expected kog.auditEvent schema version 1.");
    }
    if (!IsOpaqueId(InEvent.eventId)) {
        AddIssue(result, "$.eventId", "invalid_event_id", "Event ID is invalid.");
    }
    if (!IsOpaqueId(InEvent.runId)) {
        AddIssue(result, "$.runId", "invalid_run_id", "Run ID is invalid.");
    }
    if (InEvent.parentRunId.has_value() && !IsOpaqueId(*InEvent.parentRunId)) {
        AddIssue(result, "$.parentRunId", "invalid_parent_run_id",
                 "Parent run ID is invalid.");
    }
    if (InEvent.parentRunId.has_value() &&
        *InEvent.parentRunId == InEvent.runId) {
        AddIssue(result, "$.parentRunId", "self_parent_run",
                 "A run cannot be its own parent.");
    }
    if (InEvent.attempt == 0) {
        AddIssue(result, "$.attempt", "invalid_attempt",
                 "Attempt must be at least one.");
    }
    if (!IsOpaqueId(InEvent.planId)) {
        AddIssue(result, "$.planId", "invalid_plan_id", "Plan ID is invalid.");
    }
    if (!IsSha256(InEvent.planSha256)) {
        AddIssue(result, "$.planSha256", "invalid_sha256",
                 "Plan hash must be lowercase SHA-256.");
    }
    if (InEvent.sequence == 0 || InEvent.sequence > kMaxSafeJsonInteger) {
        AddIssue(result, "$.sequence", "invalid_sequence",
                 "Sequence must be a positive JSON-safe integer.");
    }
    ValidateTimestampRange(InEvent.startedAtUtc, InEvent.finishedAtUtc, "$",
                           result);
    ValidateRepositoryTransition(InEvent.repository, "$.repository", result);
    if (!IsSemanticToken(InEvent.phase)) {
        AddIssue(result, "$.phase", "invalid_phase",
                 "Phase must be a bounded semantic token.");
    }
    if (!IsSemanticToken(InEvent.action)) {
        AddIssue(result, "$.action", "invalid_action",
                 "Action must be semantic and must not contain command text.");
    }
    ValidateOutcome(InEvent.outcome, "$.outcome", result);
    ValidateCorrelation(InEvent.correlation, "$.correlation", result);
    ValidateReferences(InEvent.policyRefs, "$.policyRefs", result);
    ValidateReferences(InEvent.approvalRefs, "$.approvalRefs", result);
    ValidateArtifacts(InEvent.artifacts, "$.artifacts", result);
    return result;
}

auto ValidateRunReceipt(const RunReceipt& InReceipt) -> ValidationResult {
    ValidationResult result;
    if (InReceipt.schemaName != kRunReceiptSchemaName ||
        InReceipt.schemaVersion != kAuditSchemaVersionV1) {
        AddIssue(result, "$", "unsupported_schema",
                 "Expected kog.runReceipt schema version 1.");
    }
    if (!IsOpaqueId(InReceipt.runId)) {
        AddIssue(result, "$.runId", "invalid_run_id", "Run ID is invalid.");
    }
    if (InReceipt.parentRunId.has_value() &&
        !IsOpaqueId(*InReceipt.parentRunId)) {
        AddIssue(result, "$.parentRunId", "invalid_parent_run_id",
                 "Parent run ID is invalid.");
    }
    if (InReceipt.parentRunId.has_value() &&
        *InReceipt.parentRunId == InReceipt.runId) {
        AddIssue(result, "$.parentRunId", "self_parent_run",
                 "A run cannot be its own parent.");
    }
    if (InReceipt.attempt == 0) {
        AddIssue(result, "$.attempt", "invalid_attempt",
                 "Attempt must be at least one.");
    }
    if (!IsOpaqueId(InReceipt.planId)) {
        AddIssue(result, "$.planId", "invalid_plan_id", "Plan ID is invalid.");
    }
    if (!IsSha256(InReceipt.planSha256)) {
        AddIssue(result, "$.planSha256", "invalid_sha256",
                 "Plan hash must be lowercase SHA-256.");
    }
    ValidateTimestampRange(InReceipt.startedAtUtc, InReceipt.finishedAtUtc, "$",
                           result);
    if (InReceipt.eventCount > kMaxEvents ||
        InReceipt.firstSequence > kMaxSafeJsonInteger ||
        InReceipt.lastSequence > kMaxSafeJsonInteger) {
        AddIssue(result, "$", "event_range_out_of_bounds",
                 "Receipt event counters exceed v1 bounds.");
    }
    if (InReceipt.eventCount == 0) {
        if (InReceipt.firstSequence != 0 || InReceipt.lastSequence != 0 ||
            InReceipt.terminalOutcome.status != OutcomeState::Unknown) {
            AddIssue(
                result, "$", "invalid_empty_run_receipt",
                "Only an explicitly unknown crash receipt may contain zero events.");
        }
        if (InReceipt.eventStreamSha256 != Sha256("")) {
            AddIssue(result, "$.eventStreamSha256",
                     "event_stream_hash_mismatch",
                     "A zero-event receipt must bind the SHA-256 of the empty JSONL "
                     "stream.");
        }
        if (!InReceipt.repositories.empty()) {
            AddIssue(result, "$.repositories",
                     "receipt_repository_unobserved",
                     "A zero-event receipt cannot claim repository transitions.");
        }
    } else if (InReceipt.firstSequence != 1 ||
               InReceipt.lastSequence != InReceipt.eventCount) {
        AddIssue(result, "$", "receipt_sequence_mismatch",
                 "Receipt sequence bounds must describe a contiguous stream "
                 "starting at one.");
    }
    if (!IsSha256(InReceipt.eventStreamSha256)) {
        AddIssue(result, "$.eventStreamSha256", "invalid_sha256",
                 "Event stream hash must be lowercase SHA-256.");
    }
    ValidateOutcome(InReceipt.terminalOutcome, "$.terminalOutcome", result);
    ValidateCorrelation(InReceipt.correlation, "$.correlation", result);
    if (InReceipt.repositories.size() > kMaxRepositories) {
        AddIssue(result, "$.repositories", "too_many_repositories",
                 "Receipt repository list exceeds the v1 bound.");
    }
    std::set<std::string> repositoryIds;
    for (std::size_t index = 0; index < InReceipt.repositories.size(); ++index) {
        const auto path = "$.repositories[" + std::to_string(index) + "]";
        ValidateRepositoryTransition(InReceipt.repositories[index], path, result);
        if (!repositoryIds.insert(InReceipt.repositories[index].repositoryId)
                 .second) {
            AddIssue(result, path + ".id", "duplicate_repository_id",
                     "Receipt repository IDs must be unique.");
        }
    }
    ValidateReferences(InReceipt.policyRefs, "$.policyRefs", result);
    ValidateReferences(InReceipt.approvalRefs, "$.approvalRefs", result);
    ValidateArtifacts(InReceipt.artifacts, "$.artifacts", result);
    return result;
}

auto ValidateRunTrace(const RunReceipt& InReceipt,
                      std::span<const AuditEvent> InEvents)
    -> ValidationResult {
    ValidationResult result = ValidateRunReceipt(InReceipt);
    if (InEvents.empty()) {
        if (InReceipt.eventCount != 0 ||
            InReceipt.terminalOutcome.status != OutcomeState::Unknown) {
            AddIssue(result, "$", "missing_terminal_trace",
                     "A non-crash receipt requires an ordered event stream.");
        }
        return result;
    }

    AppendIssues(result, ValidateEventStream(InEvents), "$.events");
    if (InReceipt.eventCount != InEvents.size() ||
        InReceipt.firstSequence != InEvents.front().sequence ||
        InReceipt.lastSequence != InEvents.back().sequence) {
        AddIssue(result, "$", "receipt_event_count_mismatch",
                 "Receipt counters do not match the supplied event stream.");
    }
    const auto canonicalEvents = SerializeAuditEventsJsonl(InEvents);
    if (!canonicalEvents.ok()) {
        AppendIssues(result, canonicalEvents.validation, "$.events");
    } else if (Sha256(canonicalEvents.json) != InReceipt.eventStreamSha256) {
        AddIssue(
            result, "$.eventStreamSha256", "event_stream_hash_mismatch",
            "Receipt event-stream hash does not match the canonical JSONL bytes.");
    }

    std::map<std::string, std::pair<RepositoryState, RepositoryState>> observed;
    std::map<std::string, AuditReference> observedPolicies;
    std::map<std::string, AuditReference> observedApprovals;
    std::map<std::string, ArtifactReference> observedArtifacts;
    const auto observeReferences =
        [&result](const std::vector<AuditReference>& InReferences,
                  std::map<std::string, AuditReference>& InOutObserved,
                  std::string_view InPath, std::string_view InKind) {
            for (std::size_t index = 0; index < InReferences.size(); ++index) {
                const auto& reference = InReferences[index];
                const auto [iterator, inserted] =
                    InOutObserved.emplace(reference.id, reference);
                if (!inserted && iterator->second != reference) {
                    AddIssue(
                        result, std::string(InPath) + "[" + std::to_string(index) + "]",
                        "evidence_equivocation",
                        std::string(InKind) +
                            " reference IDs must bind one immutable hash per run.");
                }
            }
        };
    const auto observeArtifacts =
        [&result](const std::vector<ArtifactReference>& InArtifacts,
                  std::map<std::string, ArtifactReference>& InOutObserved,
                  std::string_view InPath) {
            for (std::size_t index = 0; index < InArtifacts.size(); ++index) {
                const auto& artifact = InArtifacts[index];
                const auto [iterator, inserted] =
                    InOutObserved.emplace(artifact.id, artifact);
                if (!inserted && iterator->second != artifact) {
                    AddIssue(
                        result, std::string(InPath) + "[" + std::to_string(index) + "]",
                        "evidence_equivocation",
                        "Artifact IDs must bind one immutable descriptor per run.");
                }
            }
        };
    for (std::size_t index = 0; index < InEvents.size(); ++index) {
        const auto& event = InEvents[index];
        const auto path = "$.events[" + std::to_string(index) + "]";
        if (event.runId != InReceipt.runId ||
            event.parentRunId != InReceipt.parentRunId ||
            event.attempt != InReceipt.attempt ||
            event.planId != InReceipt.planId ||
            event.planSha256 != InReceipt.planSha256 ||
            event.correlation != InReceipt.correlation) {
            AddIssue(result, path, "receipt_identity_mismatch",
                     "Event identity does not match the receipt.");
        }
        if (event.startedAtUtc < InReceipt.startedAtUtc ||
            event.finishedAtUtc > InReceipt.finishedAtUtc) {
            AddIssue(result, path, "event_outside_run_time",
                     "Event timestamps must be enclosed by the receipt.");
        }

        const auto iterator = observed.find(event.repository.repositoryId);
        if (iterator == observed.end()) {
            observed.emplace(
                event.repository.repositoryId,
                std::make_pair(event.repository.before, event.repository.after));
        } else {
            if (iterator->second.second != event.repository.before) {
                AddIssue(result, path + ".repository.before",
                         "broken_repository_transition",
                         "Each repository transition must chain from the prior after "
                         "state.");
            }
            iterator->second.second = event.repository.after;
        }
        observeReferences(event.policyRefs, observedPolicies, path + ".policyRefs",
                          "Policy");
        observeReferences(event.approvalRefs, observedApprovals,
                          path + ".approvalRefs", "Approval");
        observeArtifacts(event.artifacts, observedArtifacts, path + ".artifacts");
    }

    std::map<std::string, RepositoryTransition> receiptRepositories;
    for (const auto& repository : InReceipt.repositories) {
        receiptRepositories.emplace(repository.repositoryId, repository);
    }
    for (const auto& [id, transition] : observed) {
        const auto iterator = receiptRepositories.find(id);
        if (iterator == receiptRepositories.end()) {
            AddIssue(
                result, "$.repositories", "receipt_repository_missing",
                "Receipt must cover every repository present in the event stream.");
            continue;
        }
        if (iterator->second.before != transition.first ||
            iterator->second.after != transition.second) {
            AddIssue(
                result, "$.repositories", "receipt_repository_state_mismatch",
                "Receipt before/after state must match the event transition chain.");
        }
    }
    for (const auto& [id, transition] : receiptRepositories) {
        (void)transition;
        if (!observed.contains(id)) {
            AddIssue(
                result, "$.repositories", "receipt_repository_unobserved",
                "Receipt cannot claim a repository absent from the event stream.");
        }
    }

    std::map<std::string, AuditReference> receiptPolicies;
    std::map<std::string, AuditReference> receiptApprovals;
    std::map<std::string, ArtifactReference> receiptArtifacts;
    for (const auto& reference : InReceipt.policyRefs) {
        receiptPolicies.emplace(reference.id, reference);
    }
    for (const auto& reference : InReceipt.approvalRefs) {
        receiptApprovals.emplace(reference.id, reference);
    }
    for (const auto& artifact : InReceipt.artifacts) {
        receiptArtifacts.emplace(artifact.id, artifact);
    }
    if (receiptPolicies != observedPolicies) {
        AddIssue(result, "$.policyRefs", "receipt_evidence_mismatch",
                 "Receipt policy references must equal the event-stream union.");
    }
    if (receiptApprovals != observedApprovals) {
        AddIssue(result, "$.approvalRefs", "receipt_evidence_mismatch",
                 "Receipt approval references must equal the event-stream union.");
    }
    if (receiptArtifacts != observedArtifacts) {
        AddIssue(result, "$.artifacts", "receipt_evidence_mismatch",
                 "Receipt artifacts must equal the event-stream union.");
    }

    if (InReceipt.terminalOutcome.status == OutcomeState::Succeeded) {
        for (const auto& event : InEvents) {
            if (event.outcome.status != OutcomeState::Succeeded) {
                AddIssue(result, "$.terminalOutcome", "contradictory_success",
                         "A succeeded receipt cannot contain a non-success event.");
                break;
            }
        }
    }
    return result;
}

auto ParseAuditEventJson(std::string_view InJson) -> AuditEventParseResult {
    auto [document, validation] = ParseDocument(InJson, "AuditEvent");
    if (!document.has_value()) {
        return {std::nullopt, std::move(validation)};
    }

    CheckObjectKeys(*document,
                    {"schemaName", "schemaVersion", "eventId", "runId",
                     "parentRunId", "attempt", "planId", "planSha256", "sequence",
                     "startedAtUtc", "finishedAtUtc", "repository", "phase",
                     "action", "outcome", "correlation", "policyRefs",
                     "approvalRefs", "artifacts"},
                    "$", false, validation);

    AuditEvent event;
    event.schemaName = RequiredString(*document, "schemaName", "$", validation);
    event.schemaVersion =
        RequiredUint32(*document, "schemaVersion", "$", validation);
    event.eventId = RequiredString(*document, "eventId", "$", validation);
    event.runId = RequiredString(*document, "runId", "$", validation);
    event.parentRunId =
        RequiredNullableString(*document, "parentRunId", "$", validation);
    event.attempt = RequiredUint32(*document, "attempt", "$", validation);
    event.planId = RequiredString(*document, "planId", "$", validation);
    event.planSha256 = RequiredString(*document, "planSha256", "$", validation);
    event.sequence = RequiredUnsigned(*document, "sequence", "$", validation);
    event.startedAtUtc =
        RequiredString(*document, "startedAtUtc", "$", validation);
    event.finishedAtUtc =
        RequiredString(*document, "finishedAtUtc", "$", validation);
    event.phase = RequiredString(*document, "phase", "$", validation);
    event.action = RequiredString(*document, "action", "$", validation);

    if (const auto* repository =
            RequiredObject(*document, "repository", "$", validation)) {
        event.repository =
            ParseRepositoryTransition(*repository, "$.repository", validation);
    }
    if (const auto* outcome =
            RequiredObject(*document, "outcome", "$", validation)) {
        event.outcome = ParseOutcome(*outcome, "$.outcome", validation);
    }
    if (const auto* correlation =
            RequiredObject(*document, "correlation", "$", validation)) {
        event.correlation =
            ParseCorrelation(*correlation, "$.correlation", validation);
    }
    if (const auto* refs =
            RequiredArray(*document, "policyRefs", "$", validation)) {
        event.policyRefs = ParseReferences(*refs, "$.policyRefs", validation);
    }
    if (const auto* refs =
            RequiredArray(*document, "approvalRefs", "$", validation)) {
        event.approvalRefs = ParseReferences(*refs, "$.approvalRefs", validation);
    }
    if (const auto* artifacts =
            RequiredArray(*document, "artifacts", "$", validation)) {
        event.artifacts = ParseArtifacts(*artifacts, "$.artifacts", validation);
    }
    AppendIssues(validation, ValidateAuditEvent(event));
    return {std::move(event), std::move(validation)};
}

auto ParseAuditEventsJsonl(std::string_view InJsonl) -> AuditEventsParseResult {
    AuditEventsParseResult result;
    if (InJsonl.empty()) {
        AddIssue(result.validation, "$", "empty_event_stream",
                 "Audit JSONL must not be empty.");
        return result;
    }
    if (InJsonl.size() > kMaxJsonlBytes) {
        AddIssue(result.validation, "$", "invalid_document_size",
                 "Audit JSONL exceeds the bounded parser size.");
        return result;
    }
    if (static_cast<std::size_t>(
            std::count(InJsonl.begin(), InJsonl.end(), '\n')) > kMaxEvents) {
        AddIssue(result.validation, "$", "too_many_events",
                 "An audit stream exceeds the v1 event bound.");
        return result;
    }
    if (InJsonl.back() != '\n') {
        AddIssue(result.validation, "$", "missing_final_lf",
                 "Audit JSONL requires exactly one LF after every record.");
        return result;
    }
    if (InJsonl.find('\r') != std::string_view::npos) {
        AddIssue(result.validation, "$", "cr_not_allowed",
                 "Audit JSONL framing uses LF, never CRLF.");
        return result;
    }
    if (InJsonl.size() >= 3 && static_cast<unsigned char>(InJsonl[0]) == 0xEFU &&
        static_cast<unsigned char>(InJsonl[1]) == 0xBBU &&
        static_cast<unsigned char>(InJsonl[2]) == 0xBFU) {
        AddIssue(result.validation, "$", "bom_not_allowed",
                 "Audit JSONL must not contain a BOM.");
        return result;
    }

    std::size_t cursor = 0;
    std::size_t lineIndex = 0;
    while (cursor < InJsonl.size()) {
        const auto lineEnd = InJsonl.find('\n', cursor);
        if (lineEnd == std::string_view::npos) {
            break;
        }
        const auto line = InJsonl.substr(cursor, lineEnd - cursor);
        if (line.empty()) {
            AddIssue(result.validation, "$[" + std::to_string(lineIndex) + "]",
                     "blank_jsonl_record",
                     "Audit JSONL cannot contain blank records.");
            result.values.clear();
            return result;
        } else {
            auto parsed = ParseAuditEventJson(line);
            AppendIssues(result.validation, parsed.validation,
                         "$[" + std::to_string(lineIndex) + "]");
            if (!parsed.ok()) {
                result.values.clear();
                return result;
            }
            result.values.push_back(std::move(*parsed.value));
        }
        cursor = lineEnd + 1;
        ++lineIndex;
    }
    AppendIssues(result.validation, ValidateEventStream(result.values, false));
    return result;
}

auto ParseRunReceiptJson(std::string_view InJson) -> RunReceiptParseResult {
    auto [document, validation] = ParseDocument(InJson, "RunReceipt");
    if (!document.has_value()) {
        return {std::nullopt, std::move(validation)};
    }

    CheckObjectKeys(*document,
                    {"schemaName", "schemaVersion", "runId", "parentRunId",
                     "attempt", "planId", "planSha256", "startedAtUtc",
                     "finishedAtUtc", "firstSequence", "lastSequence",
                     "eventCount", "eventStreamSha256", "terminalOutcome",
                     "correlation", "repositories", "policyRefs", "approvalRefs",
                     "artifacts"},
                    "$", false, validation);

    RunReceipt receipt;
    receipt.schemaName = RequiredString(*document, "schemaName", "$", validation);
    receipt.schemaVersion =
        RequiredUint32(*document, "schemaVersion", "$", validation);
    receipt.runId = RequiredString(*document, "runId", "$", validation);
    receipt.parentRunId =
        RequiredNullableString(*document, "parentRunId", "$", validation);
    receipt.attempt = RequiredUint32(*document, "attempt", "$", validation);
    receipt.planId = RequiredString(*document, "planId", "$", validation);
    receipt.planSha256 = RequiredString(*document, "planSha256", "$", validation);
    receipt.startedAtUtc =
        RequiredString(*document, "startedAtUtc", "$", validation);
    receipt.finishedAtUtc =
        RequiredString(*document, "finishedAtUtc", "$", validation);
    receipt.firstSequence =
        RequiredUnsigned(*document, "firstSequence", "$", validation);
    receipt.lastSequence =
        RequiredUnsigned(*document, "lastSequence", "$", validation);
    receipt.eventCount =
        RequiredUnsigned(*document, "eventCount", "$", validation);
    receipt.eventStreamSha256 =
        RequiredString(*document, "eventStreamSha256", "$", validation);

    if (const auto* outcome =
            RequiredObject(*document, "terminalOutcome", "$", validation)) {
        receipt.terminalOutcome =
            ParseOutcome(*outcome, "$.terminalOutcome", validation);
    }
    if (const auto* correlation =
            RequiredObject(*document, "correlation", "$", validation)) {
        receipt.correlation =
            ParseCorrelation(*correlation, "$.correlation", validation);
    }
    if (const auto* repositories =
            RequiredArray(*document, "repositories", "$", validation)) {
        if (repositories->size() > kMaxRepositories) {
            AddIssue(validation, "$.repositories", "too_many_repositories",
                     "Receipt repository list exceeds the v1 bound.");
        } else {
            receipt.repositories.reserve(repositories->size());
            for (std::size_t index = 0; index < repositories->size(); ++index) {
                const auto path = "$.repositories[" + std::to_string(index) + "]";
                if (!(*repositories)[index].is_object()) {
                    AddIssue(validation, path, "expected_object",
                             "Expected a repository transition.");
                    continue;
                }
                receipt.repositories.push_back(ParseRepositoryTransition(
                    (*repositories)[index], path, validation));
            }
        }
    }
    if (const auto* refs =
            RequiredArray(*document, "policyRefs", "$", validation)) {
        receipt.policyRefs = ParseReferences(*refs, "$.policyRefs", validation);
    }
    if (const auto* refs =
            RequiredArray(*document, "approvalRefs", "$", validation)) {
        receipt.approvalRefs = ParseReferences(*refs, "$.approvalRefs", validation);
    }
    if (const auto* artifacts =
            RequiredArray(*document, "artifacts", "$", validation)) {
        receipt.artifacts = ParseArtifacts(*artifacts, "$.artifacts", validation);
    }
    AppendIssues(validation, ValidateRunReceipt(receipt));
    return {std::move(receipt), std::move(validation)};
}

auto SerializeAuditEventJson(const AuditEvent& InEvent) -> SerializationResult {
    SerializationResult result;
    result.validation = ValidateAuditEvent(InEvent);
    if (result.validation.ok()) {
        try {
            result.json =
                ToJson(InEvent).dump(-1, ' ', false, Json::error_handler_t::strict);
        } catch (const Json::exception&) {
            AddIssue(result.validation, "$", "serialization_error",
                     "The audit event contains data that cannot be encoded as "
                     "canonical UTF-8 JSON.");
        }
        if (result.json.size() > kMaxDocumentBytes) {
            result.json.clear();
            AddIssue(result.validation, "$", "invalid_document_size",
                     "Canonical AuditEvent JSON exceeds the 4 MiB v1 bound.");
        }
    }
    return result;
}

auto SerializeRunReceiptJson(const RunReceipt& InReceipt)
    -> SerializationResult {
    SerializationResult result;
    result.validation = ValidateRunReceipt(InReceipt);
    if (result.validation.ok()) {
        try {
            result.json =
                ToJson(InReceipt).dump(-1, ' ', false, Json::error_handler_t::strict);
        } catch (const Json::exception&) {
            AddIssue(result.validation, "$", "serialization_error",
                     "The run receipt contains data that cannot be encoded as "
                     "canonical UTF-8 JSON.");
        }
        if (result.json.size() > kMaxDocumentBytes) {
            result.json.clear();
            AddIssue(result.validation, "$", "invalid_document_size",
                     "Canonical RunReceipt JSON exceeds the 4 MiB v1 bound.");
        }
    }
    return result;
}

auto SerializeAuditEventsJsonl(std::span<const AuditEvent> InEvents)
    -> SerializationResult {
    SerializationResult result;
    result.validation = ValidateEventStream(InEvents);
    if (!result.validation.ok()) {
        return result;
    }
    for (const auto& event : InEvents) {
        std::string record;
        try {
            record =
                ToJson(event).dump(-1, ' ', false, Json::error_handler_t::strict);
        } catch (const Json::exception&) {
            result.json.clear();
            AddIssue(result.validation, "$", "serialization_error",
                     "The audit stream contains data that cannot be encoded as "
                     "canonical UTF-8 JSON.");
            return result;
        }
        if (record.size() >= kMaxJsonlBytes ||
            result.json.size() > kMaxJsonlBytes - record.size() - 1U) {
            result.json.clear();
            AddIssue(result.validation, "$", "invalid_document_size",
                     "Canonical audit JSONL exceeds the 64 MiB v1 bound.");
            return result;
        }
        result.json += record;
        result.json.push_back('\n');
    }
    return result;
}

} // namespace kano::git::audit
