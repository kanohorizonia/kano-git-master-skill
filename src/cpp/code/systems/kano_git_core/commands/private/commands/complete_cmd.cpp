// __complete command - internal completion backend

#include "command_registry.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

bool StartsWith(const std::string& InValue, const std::string& InPrefix) {
    return InValue.rfind(InPrefix, 0) == 0;
}

const CLI::App* FindExactSubcommand(const CLI::App* InContext, const std::string& InToken) {
    const auto subs = InContext->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (const CLI::App* sub : subs) {
        if (sub->get_name() == InToken) {
            return sub;
        }
    }
    return nullptr;
}

void AddMatchingSubcommands(const CLI::App* InContext,
                            const std::string& InPrefix,
                            std::set<std::string>& InOut) {
    const auto subs = InContext->get_subcommands([](const CLI::App* sub) {
        return sub != nullptr && !sub->get_name().empty();
    });

    for (const CLI::App* sub : subs) {
        const std::string& name = sub->get_name();
        if (InPrefix.empty() || StartsWith(name, InPrefix)) {
            InOut.insert(name);
        }
    }
}

void AddMatchingOptions(const CLI::App* InContext,
                        const std::string& InPrefix,
                        std::set<std::string>& InOut) {
    const auto opts = InContext->get_options([](const CLI::Option* opt) {
        return opt != nullptr && opt->nonpositional();
    });

    for (const CLI::Option* opt : opts) {
        for (const std::string& lname : opt->get_lnames()) {
            const std::string full = "--" + lname;
            if (InPrefix.empty() || StartsWith(full, InPrefix)) {
                InOut.insert(full);
            }
        }

        for (const std::string& sname : opt->get_snames()) {
            const std::string full = "-" + sname;
            if (InPrefix.empty() || StartsWith(full, InPrefix)) {
                InOut.insert(full);
            }
        }
    }
}

std::vector<std::string> CompleteFromTokens(const CLI::App& InRoot,
                                            const std::vector<std::string>& InTokens) {
    std::vector<std::string> contextTokens;
    std::string partial;

    if (!InTokens.empty()) {
        contextTokens.assign(InTokens.begin(), InTokens.end() - 1);
        partial = InTokens.back();
    }

    const CLI::App* context = &InRoot;
    for (const std::string& token : contextTokens) {
        if (token.empty() || token[0] == '-') {
            break;
        }

        const CLI::App* next = FindExactSubcommand(context, token);
        if (next == nullptr) {
            break;
        }
        context = next;
    }

    std::set<std::string> candidates;

    if (!partial.empty() && partial[0] == '-') {
        AddMatchingOptions(context, partial, candidates);
    } else {
        AddMatchingSubcommands(context, partial, candidates);
        AddMatchingOptions(context, partial, candidates);
    }

    return std::vector<std::string>(candidates.begin(), candidates.end());
}

} // namespace

void RegisterComplete(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("__complete", "Internal completion backend");
    cmd->group("");

    auto* context = new std::vector<std::string>{};
    auto* current = new std::string{};

    cmd->add_option("--context", *context, "Completion context token (repeatable)");
    cmd->add_option("--current", *current, "Current partial token")->default_str("");

    cmd->callback([&InApp, context, current]() {
        std::vector<std::string> tokens = *context;
        tokens.push_back(*current);
        const auto candidates = CompleteFromTokens(InApp, tokens);

        for (const std::string& candidate : candidates) {
            std::cout << candidate << '\n';
        }
    });
}

} // namespace kano::git::commands
