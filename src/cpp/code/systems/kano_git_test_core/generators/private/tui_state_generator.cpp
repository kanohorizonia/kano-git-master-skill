#include "tui_state_generator.hpp"
#include <random>
#include <algorithm>

namespace tui_test {

// Random TUI State Generator Implementation
RandomTuiStateGenerator::RandomTuiStateGenerator(size_t seed) {
    current_state_ = generate_random_state();
}

auto RandomTuiStateGenerator::get() const -> const TuiState& {
    return current_state_;
}

auto RandomTuiStateGenerator::next() -> bool {
    if (++iteration_ >= max_iterations_) {
        return false;
    }
    current_state_ = generate_random_state();
    return true;
}

auto RandomTuiStateGenerator::generate_random_state() -> TuiState {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    TuiState state;
    
    // Random mode
    std::uniform_int_distribution<> mode_dist(0, 4);
    state.mode = static_cast<TuiMode>(mode_dist(gen));
    
    // Random buffer (0-50 characters)
    std::uniform_int_distribution<> len_dist(0, 50);
    size_t buffer_len = len_dist(gen);
    
    std::uniform_int_distribution<> char_dist('a', 'z');
    state.buffer.reserve(buffer_len);
    for (size_t i = 0; i < buffer_len; ++i) {
        // Mix of letters, spaces, and special chars
        if (i > 0 && i % 10 == 0) {
            state.buffer += ' ';
        } else if (i > 0 && i % 15 == 0) {
            state.buffer += '-';
        } else {
            state.buffer += static_cast<char>(char_dist(gen));
        }
    }
    
    // Random cursor position (always valid)
    if (!state.buffer.empty()) {
        std::uniform_int_distribution<> cursor_dist(0, state.buffer.length());
        state.cursor_pos = cursor_dist(gen);
    } else {
        state.cursor_pos = 0;
    }
    
    // Random candidates (0-15 items)
    std::uniform_int_distribution<> cand_count_dist(0, 15);
    size_t cand_count = cand_count_dist(gen);
    
    for (size_t i = 0; i < cand_count; ++i) {
        std::string candidate = "cmd_" + std::to_string(i);
        state.candidates.push_back(candidate);
    }
    
    // Random selected candidate (always valid if candidates exist)
    if (!state.candidates.empty()) {
        std::uniform_int_distribution<> sel_dist(0, state.candidates.size() - 1);
        state.selected_candidate = sel_dist(gen);
        state.show_candidates = true;
    } else {
        state.selected_candidate = 0;
        state.show_candidates = false;
    }
    
    // Random footer message
    std::uniform_int_distribution<> msg_dist(0, 3);
    switch (msg_dist(gen)) {
        case 0: state.footer_message = ""; break;
        case 1: state.footer_message = "Command executed successfully"; break;
        case 2: state.footer_message = "Unknown command"; state.footer_is_error = true; break;
        case 3: state.footer_message = "Invalid syntax"; state.footer_is_error = true; break;
    }
    
    return state;
}

// Random Buffer State Generator Implementation
RandomBufferStateGenerator::RandomBufferStateGenerator() {
    current_state_ = generate_random_buffer();
}

auto RandomBufferStateGenerator::get() const -> const BufferState& {
    return current_state_;
}

auto RandomBufferStateGenerator::next() -> bool {
    if (++iteration_ >= max_iterations_) {
        return false;
    }
    current_state_ = generate_random_buffer();
    return true;
}

auto RandomBufferStateGenerator::generate_random_buffer() -> BufferState {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    BufferState state;
    
    // Random buffer length (0-100 characters)
    std::uniform_int_distribution<> len_dist(0, 100);
    size_t buffer_len = len_dist(gen);
    
    // Generate random text
    std::uniform_int_distribution<> char_dist('a', 'z');
    state.text.reserve(buffer_len);
    for (size_t i = 0; i < buffer_len; ++i) {
        state.text += static_cast<char>(char_dist(gen));
    }
    
    // Random cursor position (always valid: 0 <= pos <= length)
    std::uniform_int_distribution<> cursor_dist(0, state.text.length());
    state.cursor_pos = cursor_dist(gen);
    
    return state;
}

// Random Candidate List Generator Implementation
RandomCandidateListGenerator::RandomCandidateListGenerator() {
    current_list_ = generate_random_list();
}

auto RandomCandidateListGenerator::get() const -> const CandidateList& {
    return current_list_;
}

auto RandomCandidateListGenerator::next() -> bool {
    if (++iteration_ >= max_iterations_) {
        return false;
    }
    current_list_ = generate_random_list();
    return true;
}

auto RandomCandidateListGenerator::generate_random_list() -> CandidateList {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    CandidateList list;
    
    // Random number of candidates (0-20)
    std::uniform_int_distribution<> count_dist(0, 20);
    size_t count = count_dist(gen);
    
    // Generate random candidate names
    for (size_t i = 0; i < count; ++i) {
        std::string candidate = "candidate_" + std::to_string(i);
        list.items.push_back(candidate);
    }
    
    // Random selected index (always valid if items exist)
    if (!list.items.empty()) {
        std::uniform_int_distribution<> sel_dist(0, list.items.size() - 1);
        list.selected_index = sel_dist(gen);
    } else {
        list.selected_index = 0;
    }
    
    return list;
}

} // namespace tui_test
