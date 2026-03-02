#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>
#include <string>
#include <vector>

namespace tui_test {

// Forward declarations for TUI types (will be implemented in actual components)
enum class TuiMode {
    Normal,
    Command,
    CommandPalette,
    Help,
    Confirm
};

struct TuiState {
    TuiMode mode = TuiMode::Normal;
    std::string buffer;
    size_t cursor_pos = 0;
    std::vector<std::string> candidates;
    int selected_candidate = 0;
    bool show_candidates = false;
    std::string footer_message;
    bool footer_is_error = false;
};

// Generator for random TUI states
class RandomTuiStateGenerator : public Catch::Generators::IGenerator<TuiState> {
public:
    RandomTuiStateGenerator(size_t seed = 0);
    
    auto get() const -> const TuiState& override;
    auto next() -> bool override;

private:
    TuiState current_state_;
    size_t iteration_ = 0;
    size_t max_iterations_ = 100;
    
    auto generate_random_state() -> TuiState;
};

// Helper function to create random TUI state generator
inline auto random_tui_state(size_t count = 100) -> Catch::Generators::GeneratorWrapper<TuiState> {
    return Catch::Generators::GeneratorWrapper<TuiState>(
        Catch::Detail::make_unique<RandomTuiStateGenerator>()
    );
}

// Generator for random buffer states with valid cursor positions
struct BufferState {
    std::string text;
    size_t cursor_pos;
};

class RandomBufferStateGenerator : public Catch::Generators::IGenerator<BufferState> {
public:
    RandomBufferStateGenerator();
    
    auto get() const -> const BufferState& override;
    auto next() -> bool override;

private:
    BufferState current_state_;
    size_t iteration_ = 0;
    size_t max_iterations_ = 100;
    
    auto generate_random_buffer() -> BufferState;
};

inline auto random_buffer_state(size_t count = 100) -> Catch::Generators::GeneratorWrapper<BufferState> {
    return Catch::Generators::GeneratorWrapper<BufferState>(
        Catch::Detail::make_unique<RandomBufferStateGenerator>()
    );
}

// Generator for random candidate lists
struct CandidateList {
    std::vector<std::string> items;
    int selected_index;
};

class RandomCandidateListGenerator : public Catch::Generators::IGenerator<CandidateList> {
public:
    RandomCandidateListGenerator();
    
    auto get() const -> const CandidateList& override;
    auto next() -> bool override;

private:
    CandidateList current_list_;
    size_t iteration_ = 0;
    size_t max_iterations_ = 100;
    
    auto generate_random_list() -> CandidateList;
};

inline auto random_candidate_list(size_t count = 100) -> Catch::Generators::GeneratorWrapper<CandidateList> {
    return Catch::Generators::GeneratorWrapper<CandidateList>(
        Catch::Detail::make_unique<RandomCandidateListGenerator>()
    );
}

} // namespace tui_test
