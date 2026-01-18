#!/bin/bash
# Sol IDE - Build Script for Unix-like systems (Linux, macOS, BSD)

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}======================================${NC}"
    echo -e "${BLUE}  Sol IDE Build System${NC}"
    echo -e "${BLUE}======================================${NC}"
    echo ""
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}! $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Detect OS
detect_os() {
    case "$(uname -s)" in
        Darwin*)    OS="macos" ;;
        Linux*)     OS="linux" ;;
        FreeBSD*)   OS="freebsd" ;;
        *)          OS="unknown" ;;
    esac
    echo "Detected OS: $OS"
}

# Detect architecture
detect_arch() {
    case "$(uname -m)" in
        x86_64)     ARCH="x64" ;;
        arm64)      ARCH="arm64" ;;
        aarch64)    ARCH="arm64" ;;
        *)          ARCH="unknown" ;;
    esac
    echo "Detected architecture: $ARCH"
}

# Check dependencies
check_deps() {
    echo ""
    echo "Checking dependencies..."
    
    # Check for CMake
    if command -v cmake &> /dev/null; then
        CMAKE_VERSION=$(cmake --version | head -n1 | cut -d' ' -f3)
        print_success "CMake found: $CMAKE_VERSION"
    else
        print_error "CMake not found. Please install CMake 3.16 or later."
        exit 1
    fi
    
    # Check for C compiler
    if command -v cc &> /dev/null; then
        CC_VERSION=$(cc --version | head -n1)
        print_success "C compiler found: $CC_VERSION"
    elif command -v gcc &> /dev/null; then
        CC_VERSION=$(gcc --version | head -n1)
        print_success "GCC found: $CC_VERSION"
    elif command -v clang &> /dev/null; then
        CC_VERSION=$(clang --version | head -n1)
        print_success "Clang found: $CC_VERSION"
    else
        print_error "No C compiler found. Please install GCC or Clang."
        exit 1
    fi
    
    # Check for Make or Ninja
    if command -v ninja &> /dev/null; then
        BUILD_SYSTEM="Ninja"
        CMAKE_GENERATOR="-G Ninja"
        print_success "Ninja found"
    elif command -v make &> /dev/null; then
        BUILD_SYSTEM="Make"
        CMAKE_GENERATOR=""
        print_success "Make found"
    else
        print_error "No build system found. Please install Make or Ninja."
        exit 1
    fi
}

# Parse arguments
BUILD_TYPE="Release"
CLEAN=false
TESTS=false
ASAN=false
VERBOSE=false
INSTALL=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -d, --debug       Build in debug mode"
    echo "  -r, --release     Build in release mode (default)"
    echo "  -c, --clean       Clean build directory before building"
    echo "  -t, --tests       Build and run tests"
    echo "  -a, --asan        Enable AddressSanitizer"
    echo "  -v, --verbose     Verbose build output"
    echo "  -i, --install     Install after building"
    echo "  -j, --jobs N      Number of parallel jobs (default: $JOBS)"
    echo "  -h, --help        Show this help message"
    echo ""
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -t|--tests)
            TESTS=true
            shift
            ;;
        -a|--asan)
            ASAN=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Main build process
print_header
detect_os
detect_arch
check_deps

BUILD_DIR="build/${OS}-${ARCH}-${BUILD_TYPE,,}"

echo ""
echo "Build configuration:"
echo "  Build type: $BUILD_TYPE"
echo "  Build directory: $BUILD_DIR"
echo "  Parallel jobs: $JOBS"
echo ""

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    print_success "Clean complete"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure CMake
echo "Configuring with CMake..."
CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    $CMAKE_GENERATOR
)

if [ "$TESTS" = true ]; then
    CMAKE_ARGS+=(-DSOL_BUILD_TESTS=ON)
fi

if [ "$ASAN" = true ]; then
    CMAKE_ARGS+=(-DSOL_ENABLE_ASAN=ON)
fi

cmake "${CMAKE_ARGS[@]}" .

if [ $? -ne 0 ]; then
    print_error "CMake configuration failed"
    exit 1
fi
print_success "Configuration complete"

# Build
echo ""
echo "Building Sol IDE..."

BUILD_ARGS=(--build "$BUILD_DIR" -j "$JOBS")

if [ "$VERBOSE" = true ]; then
    BUILD_ARGS+=(--verbose)
fi

cmake "${BUILD_ARGS[@]}"

if [ $? -ne 0 ]; then
    print_error "Build failed"
    exit 1
fi

print_success "Build complete!"

# Run tests if requested
if [ "$TESTS" = true ]; then
    echo ""
    echo "Running tests..."
    cd "$BUILD_DIR"
    ctest --output-on-failure
    cd - > /dev/null
fi

# Install if requested
if [ "$INSTALL" = true ]; then
    echo ""
    echo "Installing..."
    cmake --install "$BUILD_DIR"
    print_success "Installation complete"
fi

# Print result
echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}  Build Successful!${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo "Binary location: $BUILD_DIR/bin/sol"
echo ""
echo "To run Sol:"
echo "  ./$BUILD_DIR/bin/sol [file/directory]"
echo ""
