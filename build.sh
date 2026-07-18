#!/usr/bin/env bash

set -e

echo "=== DrumCraker VST3 Build Script ==="

# Detect operating system
if [[ "$OSTYPE" == "darwin"* ]]; then
    OS_NAME="macOS"
    echo "Detected OS: macOS"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS_NAME="Linux"
    echo "Detected OS: Linux"
elif [[ "$OSTYPE" == "freebsd"* ]]; then
    OS_NAME="FreeBSD"
    echo "Detected OS: FreeBSD"
else
    echo "Warning: Unknown OS type: $OSTYPE"
    OS_NAME="Unknown"
fi

# Check Linux dependencies
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "Checking Linux dependencies..."
    MISSING_DEPS=()
    
    # Check for required packages
    if ! pkg-config --exists freetype2; then
        MISSING_DEPS+=("libfreetype6-dev")
    fi
    if ! pkg-config --exists fontconfig; then
        MISSING_DEPS+=("libfontconfig1-dev")
    fi
    if ! pkg-config --exists x11; then
        MISSING_DEPS+=("libx11-dev")
    fi
    if ! pkg-config --exists xinerama; then
        MISSING_DEPS+=("libxinerama-dev")
    fi
    if ! pkg-config --exists xrandr; then
        MISSING_DEPS+=("libxrandr-dev")
    fi
    if ! pkg-config --exists xcursor; then
        MISSING_DEPS+=("libxcursor-dev")
    fi
    if ! pkg-config --exists gtk+-3.0; then
        MISSING_DEPS+=("libgtk-3-dev")
    fi
    
    if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
        echo ""
        echo "ERROR: Missing required dependencies for JUCE on Linux"
        echo "Please install the following packages:"
        echo ""
        echo "  sudo apt-get install ${MISSING_DEPS[@]}"
        echo ""
        exit 1
    fi
    
    echo "All dependencies found!"
fi

# Check FreeBSD dependencies
if [[ "$OSTYPE" == "freebsd"* ]]; then
    echo "Checking FreeBSD dependencies..."
    MISSING_DEPS=()
    
    # Check for required packages using pkg info (more reliable than pkg-config on FreeBSD)
    REQUIRED_PKGS=("cmake" "pkgconf" "alsa-lib" "freetype2" "libX11" "libXext" "libXinerama" "libXrandr" "libXcursor" "mesa-libs" "libglvnd" "libxkbcommon" "jackit" "lv2")
    
    for pkg in "${REQUIRED_PKGS[@]}"; do
        if ! pkg info -e "$pkg" > /dev/null 2>&1; then
            MISSING_DEPS+=("$pkg")
        fi
    done
    
    if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
        echo ""
        echo "ERROR: Missing required dependencies for JUCE on FreeBSD"
        echo "Please install the following packages:"
        echo ""
        echo "  pkg install ${MISSING_DEPS[@]}"
        echo ""
        exit 1
    fi
    
    echo "All dependencies found!"
fi

# Check if JUCE is cloned
if [ ! -d "JUCE" ]; then
    echo "Cloning JUCE Framework for $OS_NAME..."
    # JUCE is cross-platform, same version works for both Linux and macOS
    git clone --depth 1 --branch 8.0.10 https://github.com/juce-framework/JUCE.git
    echo "JUCE Framework cloned successfully"
else
    echo "JUCE Framework already exists, skipping download"
fi

# Clean previous build
echo "Cleaning build directory..."
rm -rf build

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Configuring project..."
NATIVE_OPTIMIZATIONS="${NATIVE_OPTIMIZATIONS:-OFF}"
echo "Native CPU optimizations: $NATIVE_OPTIMIZATIONS"
if [[ "$OSTYPE" == "darwin"* ]]; then
    MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-12.0}"
    echo "macOS deployment target: $MACOS_DEPLOYMENT_TARGET"
    cmake .. -DCMAKE_BUILD_TYPE=Release \
        -DDRUMCRAKER_ENABLE_NATIVE_OPTIMIZATIONS="$NATIVE_OPTIMIZATIONS" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
else
    cmake .. -DCMAKE_BUILD_TYPE=Release \
        -DDRUMCRAKER_ENABLE_NATIVE_OPTIMIZATIONS="$NATIVE_OPTIMIZATIONS"
fi

# Compile
echo "Compiling..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    CPU_COUNT=$(sysctl -n hw.ncpu)
elif [[ "$OSTYPE" == "freebsd"* ]]; then
    CPU_COUNT=$(sysctl -n hw.ncpu)
else
    CPU_COUNT=$(nproc)
fi
cmake --build . --config Release -j$CPU_COUNT

# Create releases directory
echo "Creating releases directory..."
cd ..
mkdir -p releases

# Find and move artifacts (handle variable CMake output paths)
echo "Locating and moving artifacts..."

# Find generated VST3
VST3_PATH=$(find build -name "DrumCraker.vst3" -type d | head -n 1)

if [ -n "$VST3_PATH" ]; then
    echo "Found VST3 at: $VST3_PATH"
    cp -r "$VST3_PATH" releases/
    
    # Copy assets to VST3 bundle
    echo "Copying resources to VST3..."
    mkdir -p releases/DrumCraker.vst3/Contents/Resources
    cp -r assets/background.png releases/DrumCraker.vst3/Contents/Resources/
else
    echo "ERROR: Could not find generated VST3 plugin!"
    exit 1
fi

# Find generated LV2
LV2_PATH=$(find build -name "DrumCraker.lv2" -type d | head -n 1)

if [ -n "$LV2_PATH" ]; then
    echo "Found LV2 at: $LV2_PATH"
    cp -r "$LV2_PATH" releases/
    echo "Moved LV2 to releases/"
else
    echo "WARNING: Could not find generated LV2 plugin. Check if LV2 dependencies were met during configuration."
fi

# Clean only build directory (keep JUCE for future builds)
echo "Cleaning temporary directory..."
rm -rf build

echo ""
echo "=== Build completed ==="
echo "Plugin located at: $(pwd)/releases/DrumCraker.vst3"
echo ""
echo "To install, run:"
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "  cp -r releases/DrumCraker.vst3 ~/Library/Audio/Plug-Ins/VST3/"
    if [ -d "releases/DrumCraker.lv2" ]; then
        echo "  cp -r releases/DrumCraker.lv2 ~/Library/Audio/Plug-Ins/LV2/"
    fi
elif [[ "$OSTYPE" == "freebsd"* ]]; then
    echo "  cp -r releases/DrumCraker.vst3 ~/.vst3/"
    if [ -d "releases/DrumCraker.lv2" ]; then
        echo "  cp -r releases/DrumCraker.lv2 ~/.lv2/"
    fi
else
    echo "  cp -r releases/DrumCraker.vst3 ~/.vst3/"
    if [ -d "releases/DrumCraker.lv2" ]; then
        echo "  cp -r releases/DrumCraker.lv2 ~/.lv2/"
    fi
fi
