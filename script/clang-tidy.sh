#!/bin/bash

# Run clang-tidy on source files
# Requires a compile_commands.json in the build directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/host_examples/vorbis_to_wav/build"

# Find clang-tidy. A pre-set $CLANG_TIDY (CI pins it to clang-tidy-18) wins over PATH discovery.
CLANG_TIDY="${CLANG_TIDY:-}"
if [ -z "$CLANG_TIDY" ]; then
    for name in clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15; do
        if command -v "$name" &> /dev/null; then
            CLANG_TIDY="$name"
            break
        fi
    done
fi

# Check Homebrew LLVM paths on macOS
if [ -z "$CLANG_TIDY" ]; then
    for path in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy; do
        if [ -x "$path" ]; then
            CLANG_TIDY="$path"
            break
        fi
    done
fi

# Validate the resolved binary up front: catches both an empty result and a bogus pre-set
# $CLANG_TIDY, instead of failing later with a bare "command not found".
if ! command -v "$CLANG_TIDY" &> /dev/null; then
    echo "Error: clang-tidy not found or not executable: '${CLANG_TIDY:-unset}'"
    exit 1
fi

# Ensure compile_commands.json exists for the main sources
if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
    echo "Generating compile_commands.json..."
    cmake -B "$BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${ROOT_DIR}/host_examples/vorbis_to_wav"
fi

# The unit tests have their own compile DB; files missing from a DB get
# guessed flags via clang-tidy's interpolation, which is fragile, so lint
# tests/ in a separate pass against its own database
TESTS_BUILD_DIR="${ROOT_DIR}/tests/build"
if [ ! -f "${TESTS_BUILD_DIR}/compile_commands.json" ]; then
    echo "Generating tests compile_commands.json..."
    cmake -B "$TESTS_BUILD_DIR" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "${ROOT_DIR}/tests"
fi

# Find all source files in our own code only (exclude forked tremor)
# Note: examples/ excluded as ESP-IDF code can't be checked without ESP-IDF headers
SOURCES=$(find "$ROOT_DIR/src" "$ROOT_DIR/host_examples" \
    \( -type d -name 'build*' \) -prune -o \
    -path '*/tremor' -prune -o \
    \( -name '*.cpp' -o -name '*.c' \) -print 2>/dev/null || true)

# Note: tests/fuzz/ excluded as the libFuzzer harness needs the C++17 fuzzer
#       toolchain, which the tests' C++14 compile DB can't provide
TEST_SOURCES=$(find "$ROOT_DIR/tests" \
    \( -type d -name 'build*' \) -prune -o \
    -path '*/fuzz' -prune -o \
    \( -name '*.cpp' -o -name '*.c' \) -print 2>/dev/null || true)

if [ -z "$SOURCES" ] && [ -z "$TEST_SOURCES" ]; then
    echo "No source files found"
    exit 0
fi

# Parse arguments
FIX_FLAG=""
if [ "$1" = "--fix" ]; then
    FIX_FLAG="--fix"
fi

echo "Running clang-tidy..."
# --warnings-as-errors keeps the exit code non-zero on any finding even if a
# repo's .clang-tidy ever loses its WarningsAsErrors line; CI relies on this.
if [ -n "$SOURCES" ]; then
    $CLANG_TIDY -p "$BUILD_DIR" --warnings-as-errors='*' $FIX_FLAG $SOURCES
fi
if [ -n "$TEST_SOURCES" ]; then
    $CLANG_TIDY -p "$TESTS_BUILD_DIR" --warnings-as-errors='*' $FIX_FLAG $TEST_SOURCES
fi
