# TUI Command Input Enhancement - Test Infrastructure

This directory contains the test infrastructure for the TUI Command Input Enhancement feature.

## Directory Structure

```
tests/
├── CMakeLists.txt              # Test build configuration
├── README.md                   # This file
├── generators/                 # Test data generators for property-based testing
│   ├── tui_state_generator.hpp
│   ├── tui_state_generator.cpp
│   ├── command_string_generator.hpp
│   └── command_string_generator.cpp
├── unit/                       # Unit tests for individual components
│   └── test_main.cpp
├── property/                   # Property-based tests using Catch2 generators
│   └── test_main.cpp
└── integration/                # Integration tests for complete workflows
    └── test_main.cpp
```

## Test Categories

### Unit Tests (`unit/`)
Unit tests verify specific examples and edge cases for individual components:
- MetadataCache component tests
- AutocompleteEngine component tests
- InputBuffer and CandidateSelection tests
- CommandExecutor tests
- Mode transition tests
- UI rendering component tests

### Property-Based Tests (`property/`)
Property tests verify universal correctness properties across randomized inputs:
- All 34 correctness properties defined in the design document
- Each property test runs a minimum of 100 iterations
- Uses Catch2 generators for randomized test data
- Tag format: `[Feature: tui-command-input-enhancement][Property N: <property_text>]`

### Integration Tests (`integration/`)
Integration tests verify complete user workflows:
- End-to-end command input and execution
- Autocomplete interaction flows
- Command palette workflows
- Help panel navigation
- Error handling and recovery

## Test Data Generators

The `generators/` directory provides reusable test data generators for property-based testing:

### `tui_state_generator.hpp/cpp`
- `random_tui_state()`: Generates random TUI states with valid invariants
- `random_buffer_state()`: Generates random buffer states with valid cursor positions
- `random_candidate_list()`: Generates random candidate lists with valid selection

### `command_string_generator.hpp/cpp`
- `random_command_string()`: Generates random command strings
- `valid_command_prefix()`: Generates valid command prefixes for autocomplete testing
- `invalid_command_string()`: Generates invalid command strings for error handling tests
- `command_with_options()`: Generates commands with random options

## Building and Running Tests

### Build all tests
```bash
cd src/cpp
cmake --preset <your-preset>
cmake --build --preset <your-preset> --target tui_unit_tests tui_property_tests tui_integration_tests
```

### Run all TUI tests
```bash
cmake --build --preset <your-preset> --target run_tui_tests
```

### Run specific test category
```bash
# Unit tests only
./build/bin/<preset>/tui_unit_tests

# Property tests only
./build/bin/<preset>/tui_property_tests

# Integration tests only
./build/bin/<preset>/tui_integration_tests
```

### Run tests with CTest
```bash
cd build/_intermediate/<preset>
ctest --output-on-failure
```

### Run specific test cases
```bash
# Run tests matching a tag
./build/bin/<preset>/tui_property_tests "[Property 1]"

# Run shell executor focused tests
./build/bin/<preset>/tui_unit_tests "[shell-executor]"

# Run tests matching a name
./build/bin/<preset>/tui_unit_tests "Mode transition"

# List all tests
./build/bin/<preset>/tui_unit_tests --list-tests
```

## Test Requirements

This test infrastructure validates the following requirements from the design document:
- **Requirement 12.1**: Input buffer display updates within 50ms
- **Requirement 12.2**: Autocomplete generates candidates within 100ms
- **Requirement 12.3**: UI refresh within 50ms

## Adding New Tests

### Adding a Unit Test
1. Create a new test file in `unit/` or add to existing file
2. Use standard Catch2 `TEST_CASE` macro
3. Reference specific requirements in test name
4. Example:
```cpp
TEST_CASE("MetadataCache extracts command metadata", "[unit][metadata]") {
    // Test implementation
}
```

### Adding a Property Test
1. Create a new test file in `property/` or add to existing file
2. Use Catch2 generators with `GENERATE` macro
3. Tag with property number from design document
4. Run minimum 100 iterations
5. Example:
```cpp
TEST_CASE("Mode transition correctness", 
          "[Feature: tui-command-input-enhancement]"
          "[Property 1: Mode transition round-trip]") {
    auto initial_state = GENERATE(take(100, random_tui_state()));
    // Property verification
}
```

### Adding an Integration Test
1. Create a new test file in `integration/` or add to existing file
2. Test complete user workflows
3. Use realistic scenarios
4. Example:
```cpp
TEST_CASE("Complete command input workflow", "[integration][workflow]") {
    // Setup TUI state
    // Simulate user input sequence
    // Verify final state
}
```

## Dependencies

- **Catch2 v3.5.2**: Modern C++ testing framework with property-based testing support
- **FTXUI**: Terminal UI framework (for integration tests)
- **CLI11**: Command-line parsing library (for metadata extraction tests)

## Notes

- All tests are built with the same compiler flags as the main project
- Tests link against the main kano_git_core library
- Property tests use deterministic random seeds for reproducibility
- Integration tests may require temporary directories for git operations
