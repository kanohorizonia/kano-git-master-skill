#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <vector>

namespace tui_test {

// Generator for random command strings
class RandomCommandStringGenerator : public Catch::Generators::IGenerator<std::string> {
public:
    RandomCommandStringGenerator();
    
    auto get() const -> const std::string& override;
    auto next() -> bool override;

private:
    std::string current_command_;
    size_t iteration_ = 0;
    size_t max_iterations_ = 100;
    
    auto generate_random_command() -> std::string;
};

inline auto random_command_string(size_t count = 100) -> Catch::Generators::GeneratorWrapper<std::string> {
    return Catch::Generators::GeneratorWrapper<std::string>(
        Catch::Detail::make_unique<RandomCommandStringGenerator>()
    );
}

// Generator for valid command prefixes
class ValidCommandPrefixGenerator : public Catch::Generators::IGenerator<std::string> {
public:
    ValidCommandPrefixGenerator();
    
    auto get() const -> const std::string& override;
    auto next() -> bool override;

private:
    std::vector<std::string> prefixes_;
    size_t current_index_ = 0;
};

inline auto valid_command_prefix() -> Catch::Generators::GeneratorWrapper<std::string> {
    return Catch::Generators::GeneratorWrapper<std::string>(
        Catch::Detail::make_unique<ValidCommandPrefixGenerator>()
    );
}

// Generator for invalid command strings
class InvalidCommandStringGenerator : public Catch::Generators::IGenerator<std::string> {
public:
    InvalidCommandStringGenerator();
    
    auto get() const -> const std::string& override;
    auto next() -> bool override;

private:
    std::vector<std::string> invalid_commands_;
    size_t current_index_ = 0;
};

inline auto invalid_command_string() -> Catch::Generators::GeneratorWrapper<std::string> {
    return Catch::Generators::GeneratorWrapper<std::string>(
        Catch::Detail::make_unique<InvalidCommandStringGenerator>()
    );
}

// Generator for command strings with options
struct CommandWithOptions {
    std::string command;
    std::vector<std::string> options;
    std::string full_string;
};

class CommandWithOptionsGenerator : public Catch::Generators::IGenerator<CommandWithOptions> {
public:
    CommandWithOptionsGenerator();
    
    auto get() const -> const CommandWithOptions& override;
    auto next() -> bool override;

private:
    CommandWithOptions current_command_;
    size_t iteration_ = 0;
    size_t max_iterations_ = 50;
    
    auto generate_command_with_options() -> CommandWithOptions;
};

inline auto command_with_options(size_t count = 50) -> Catch::Generators::GeneratorWrapper<CommandWithOptions> {
    return Catch::Generators::GeneratorWrapper<CommandWithOptions>(
        Catch::Detail::make_unique<CommandWithOptionsGenerator>()
    );
}

} // namespace tui_test
