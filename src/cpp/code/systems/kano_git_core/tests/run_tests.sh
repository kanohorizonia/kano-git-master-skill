#!/usr/bin/env bash
# Quick script to build and run TUI command input enhancement tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

# Default preset (can be overridden)
PRESET="${1:-linux-ninja-gcc}"

echo "Building TUI tests with preset: $PRESET"
cd "$CPP_ROOT"

# Configure if needed
if [ ! -d "build/_intermediate/$PRESET" ]; then
    echo "Configuring CMake with preset $PRESET..."
    cmake --preset "$PRESET"
fi

# Build test targets
echo "Building test targets..."
cmake --build --preset "$PRESET" --target tui_unit_tests tui_property_tests tui_integration_tests

# Run tests
echo ""
echo "Running unit tests..."
"./build/bin/$PRESET/tui_unit_tests"

echo ""
echo "Running shell executor focused tests..."
"./build/bin/$PRESET/tui_unit_tests" "[shell-executor]"

echo ""
echo "Running property tests..."
"./build/bin/$PRESET/tui_property_tests"

echo ""
echo "Running integration tests..."
"./build/bin/$PRESET/tui_integration_tests"

echo ""
echo "All tests completed successfully!"
