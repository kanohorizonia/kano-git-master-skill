// meta command - exports machine-readable command metadata

#include <CLI/CLI.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

std::string EscapeJson(const std::string& InValue) {
    std::ostringstream out;
    for (unsigned char ch : InValue) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u"
                        << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string Quote(const std::string& InValue) {
    return "\"" + EscapeJson(InValue) + "\"";
}

std::string UtcNowIso8601() {
    using clock = std::chrono::system_clock;
    const std::time_t now = clock::to_time_t(clock::now());

    std::tm tmUtc{};
#if defined(_WIN32)
    gmtime_s(&tmUtc, &now);
#else
    gmtime_r(&now, &tmUtc);
#endif

    std::ostringstream out;
    out << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool OptionTakesValue(const CLI::Option* InOption) {
    return InOption->get_items_expected_max() != 0;
}

bool OptionIsMultiValue(const CLI::Option* InOption) {
    return InOption->get_expected_max() > 1 || InOption->get_items_expected_max() > 1;
}

void AppendStringArray(std::ostringstream& InOut,
                       const std::vector<std::string>& InValues,
                       const char* InPrefix) {
    InOut << '[';
    for (std::size_t i = 0; i < InValues.size(); ++i) {
        if (i > 0) {
            InOut << ',';
        }
        InOut << Quote(std::string(InPrefix) + InValues[i]);
    }
    InOut << ']';
}

void AppendOptionJson(std::ostringstream& InOut, const CLI::Option* InOption) {
    InOut << '{';
    InOut << "\"name\":" << Quote(InOption->get_name(false, false)) << ',';

    InOut << "\"long\":";
    AppendStringArray(InOut, InOption->get_lnames(), "--");
    InOut << ',';

    InOut << "\"short\":";
    AppendStringArray(InOut, InOption->get_snames(), "-");
    InOut << ',';

    InOut << "\"description\":" << Quote(InOption->get_description()) << ',';
    InOut << "\"takes_value\":" << (OptionTakesValue(InOption) ? "true" : "false") << ',';
    InOut << "\"required\":" << (InOption->get_required() ? "true" : "false") << ',';
    InOut << "\"multi\":" << (OptionIsMultiValue(InOption) ? "true" : "false");

    const std::string defaultValue = InOption->get_default_str();
    if (!defaultValue.empty()) {
        InOut << ",\"default_value\":" << Quote(defaultValue);
    }

    InOut << '}';
}

void AppendCommandJson(std::ostringstream& InOut,
                       const CLI::App* InCommand,
                       const std::string& InPath) {
    InOut << '{';
    InOut << "\"name\":" << Quote(InCommand->get_name()) << ',';
    InOut << "\"path\":" << Quote(InPath) << ',';
    InOut << "\"description\":" << Quote(InCommand->get_description()) << ',';
    InOut << "\"allow_extras\":" << (InCommand->get_allow_extras() ? "true" : "false") << ',';

    InOut << "\"options\":[";
    const auto options = InCommand->get_options([](const CLI::Option* opt) {
        return opt != nullptr && opt->nonpositional();
    });
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i > 0) {
            InOut << ',';
        }
        AppendOptionJson(InOut, options[i]);
    }
    InOut << "],";

    InOut << "\"subcommands\":[";
    const auto subcommands = InCommand->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (std::size_t i = 0; i < subcommands.size(); ++i) {
        if (i > 0) {
            InOut << ',';
        }
        const std::string childPath = InPath.empty()
            ? subcommands[i]->get_name()
            : InPath + " " + subcommands[i]->get_name();
        AppendCommandJson(InOut, subcommands[i], childPath);
    }
    InOut << ']';
    InOut << '}';
}

std::string BuildMetadataJson(const CLI::App& InRoot) {
    std::ostringstream out;
    out << '{';
    out << "\"schema_version\":\"v1\",";
    out << "\"generated_at\":" << Quote(UtcNowIso8601()) << ',';
    out << "\"binary\":" << Quote(InRoot.get_name()) << ',';
    out << "\"commands\":[";

    const auto commands = InRoot.get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        AppendCommandJson(out, commands[i], commands[i]->get_name());
    }

    out << "]}";
    return out.str();
}

} // namespace

void RegisterMeta(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("meta", "Export machine-readable command metadata");

    auto* format = new std::string{"json"};
    cmd->add_option("--format", *format, "Output format (json)")->default_str("json");

    cmd->callback([&InApp, format]() {
        if (*format != "json") {
            std::cerr << "Unsupported format: " << *format << " (expected: json)\n";
            std::exit(2);
        }

        std::cout << BuildMetadataJson(InApp) << '\n';
    });
}

} // namespace kano::git::commands
