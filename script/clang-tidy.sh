#!/bin/bash

# Run clang-tidy on source files
# Requires a compile_commands.json in the build directory

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${ROOT_DIR}/host_examples/vorbis_to_wav/build"

# Find clang-tidy
CLANG_TIDY=""
for name in clang-tidy-18 clang-tidy-17 clang-tidy-16 clang-tidy-15 clang-tidy; do
    if command -v "$name" &> /dev/null; then
        CLANG_TIDY="$name"
        break
    fi
done

# Check Homebrew LLVM paths on macOS
if [ -z "$CLANG_TIDY" ]; then
    for path in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy; do
        if [ -x "$path" ]; then
            CLANG_TIDY="$path"
            break
        fi
    done
fi

if [ -z "$CLANG_TIDY" ]; then
    echo "Error: clang-tidy not found"
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
if [ -n "$SOURCES" ]; then
    $CLANG_TIDY -p "$BUILD_DIR" $FIX_FLAG $SOURCES
fi
if [ -n "$TEST_SOURCES" ]; then
    $CLANG_TIDY -p "$TESTS_BUILD_DIR" $FIX_FLAG $TEST_SOURCES
fi
