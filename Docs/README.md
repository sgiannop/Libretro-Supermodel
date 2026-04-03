# Supermodel (Sega Model 3) - Libretro Port (Modernized)

A modernized fork of the Sega Model 3 (Supermodel) Libretro core, optimized for modern Linux distributions and updated to C++17 standards.

## 🚀 Key Improvements
- **Unified Makefile:** Single build configuration supporting 6 platforms (Linux, Windows, macOS, Android, RPi64, aarch64) following libretro/skeletor standards.
- **Platform Auto-Detection:** Automatic platform detection with sensible defaults; platform-specific source filtering for incompatible features.
- **Native Libretro Audio:** Removed legacy SDL audio dependency in favor of native `audio_batch_cb` synchronization at fixed 57.53Hz.
- **C++17 Migration:** Replaced legacy SDL-based threading and synchronization with native C++17 `std::mutex`, `std::lock_guard`, and atomic operations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase specifically for modern GCC versions.
- **Synchronous A/V Timing:** Coupled the emulator's internal hardware clock (57.53Hz) with Libretro's timing engine.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box with improved deadzone handling.
- **Configurable Service & Test Buttons:** Service and Test buttons are now mappable through the RetroArch input configuration.
- **Force Feedback / Rumble:** Full force feedback support for steering wheel games via the Libretro rumble interface.
- **Widescreen Hack:** Optional widescreen mode exposed as a core option in the RetroArch UI.
- **Libretro Portability:** Remapped configuration, NVRAM, and asset paths to follow official Libretro standards (`system` and `save` directories).
- **No External GL Dependency:** GLEW replaced with `glsym` from libretro-common — no system GL extension library required on any platform.
- **Android Support:** Full NDK integration with architecture-specific optimization (arm64, arm with NEON, x86_64, x86) and OpenGL ES 3.0.
- **macOS Universal Binary:** Builds for both Intel (x86_64) and Apple Silicon (arm64) via osxcross with automatic CPU tuning.
- **Raspberry Pi Optimized:** GLES3 rendering with CPU-specific tuning for RPi5 (Cortex-A76), RPi4 (Cortex-A72), and generic aarch64.
- **Windows Support:** Full cross-platform support with dedicated Windows build targets using MinGW — no vendored prebuilt libraries required.

## 📂 Required Assets
To run the core, you must place the emulator's configuration files in your RetroArch system directory. The core follows standard Libretro conventions and will look for assets in the following location:

* **Path:** `[RetroArch System Directory]/supermodel/Config/`
* **Required Files:**
    * `Games.xml`
    * `Supermodel.ini`

*Note: If these files are missing, the core will fail to initialize the game list and settings.*

## 🛠 Build Instructions

**Unified Makefile System:** This core uses a single unified `Makefile` supporting 6 platforms:
- **Linux/Unix** (native)
- **Windows** (MinGW cross-compile or native MSYS2)
- **macOS** (osxcross universal binary: x86_64 + arm64)
- **Android** (NDK: arm64, arm, x86_64, x86)
- **Raspberry Pi 64-bit** (RPi5, RPi4, generic aarch64)
- **Generic aarch64** (ARM64 Linux)

### Quick Start (All Platforms)

```bash
# Linux (default platform)
make -j$(nproc)

# Windows (cross-compile from Linux)
make platform=win -j$(nproc)

# macOS (osxcross from Linux)
make platform=osx -j$(nproc)

# Android (all architectures default to arm64)
make platform=android -j$(nproc)

# Raspberry Pi 64-bit
make platform=rpi64 -j$(nproc)

# Generic aarch64
make platform=aarch64 -j$(nproc)

# Debug build (any platform)
make DEBUG=1 platform=<platform> -j$(nproc)

# Clean build artifacts
make clean
```

---

### Linux (Native Build)

#### 1. Install Dependencies (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential libgl1-mesa-dev libglu1-mesa-dev zlib1g-dev
```

#### 2. Compile
```bash
make -j$(nproc)
```

#### 3. Install
```bash
cp supermodel_libretro.so ~/.config/retroarch/cores/
```

---

### Windows

You can build the core either natively on Windows or via cross-compilation from Linux.

#### Option A: Native Windows Build (MSYS2)

1. **Install MSYS2**: Download and install from [msys2.org](https://www.msys2.org/).
2. **Open the "MSYS2 MinGW 64-bit" terminal** (avoid the default MSYS terminal).
3. **Install Dependencies**:
    ```bash
    pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-make mingw-w64-x86_64-zlib
    ```
4. **Compile**:
    ```bash
    make platform=win -j%NUMBER_OF_PROCESSORS%
    ```

#### Option B: Cross-Compilation (from Linux)

1. **Install the MinGW-w64 toolchain**:
    ```bash
    sudo apt update
    sudo apt install binutils-mingw-w64-x86-64 g++-mingw-w64-x86-64 libz-mingw-w64-dev
    ```
2. **Compile**:
    ```bash
    make platform=win -j$(nproc)
    ```

**Output:** `supermodel_libretro.dll` → Copy to RetroArch `cores/` directory.

---

### macOS (Intel & Apple Silicon Universal Binary)

This core generates a universal binary supporting both Intel (x86_64) and Apple Silicon (arm64 M1/M2/M3+).

#### Option A: Native macOS Build

If building on a macOS system with Xcode installed:

```bash
# Install dependencies via Homebrew
brew install zlib

# Compile (generates universal binary)
make platform=osx -j$(sysctl -n hw.ncpu)

# Install
cp supermodel_libretro.dylib ~/.config/retroarch/cores/
```

#### Option B: Cross-Compilation from Linux (Recommended for CI/CD)

1. **Install osxcross toolchain** (to `/opt/osxcross`):
    ```bash
    sudo mkdir -p /opt
    sudo git clone https://github.com/tpoechtrager/osxcross /opt/osxcross
    cd /opt/osxcross
    
    # Download macOS SDK (follow osxcross documentation for MacOSX12.0.sdk)
    wget -nc https://github.com/rtrussell/osxcross-build/releases/download/12.0/MacOSX12.0.sdk.tar.xz
    mv MacOSX12.0.sdk.tar.xz tarballs/
    
    # Build osxcross
    sudo ./build.sh
    ```

2. **Compile** (Makefile auto-detects osxcross):
    ```bash
    # Universal binary (x86_64 + arm64)
    make platform=osx -j$(nproc)
    
    # With debug symbols
    make platform=osx DEBUG=1 -j$(nproc)
    ```
    
    The Makefile automatically:
    - Detects osxcross at `/opt/osxcross`
    - Uses `o64-clang`/`o64-clang++` compilers
    - Configures SDK and deployment target
    - Generates universal binary for both architectures
    
    To override osxcross location:
    ```bash
    make platform=osx OSXCROSS_ROOT=/custom/path -j$(nproc)
    ```

3. **Transfer to macOS** (if building on Linux):
    ```bash
    scp supermodel_libretro.dylib user@mac:~/Downloads/
    # On Mac: cp ~/Downloads/supermodel_libretro.dylib ~/.config/retroarch/cores/
    ```

**Build Details:**
- **Deployment Target:** macOS 10.15 (Catalina) and newer
- **Architecture:** Universal binary (x86_64 + arm64)
- **Renderer:** Modern OpenGL 3.2+ (Legacy3D excluded)
- **Build Time:** ~90 seconds on 4 cores

---

### Android (NDK)

Requires Android NDK. The Makefile checks `~/Android/Sdk/ndk/28.2.13676358` by default.

```bash
# arm64 (default and recommended)
make platform=android -j$(nproc)

# 32-bit ARM (armv7-a with NEON)
make platform=android arch=arm -j$(nproc)

# x86_64
make platform=android arch=x86_64 -j$(nproc)

# x86 (32-bit)
make platform=android arch=x86 -j$(nproc)

# Custom NDK path
NDK_ROOT=/path/to/ndk make platform=android -j$(nproc)
```

**Build Features:**
- Automatic NEON optimization for ARM architectures
- Position-independent code (PIC) for all binaries
- Static C++ runtime linking
- API level 24 (NDK Clang toolchain)
- GLES 3.0 rendering support

**Output:** `supermodel_libretro_android.so` → Copy to Android RetroArch cores directory (usually `/data/data/com.retroarch/cores/`)

---

### Raspberry Pi 64-bit (RPi5, RPi4, aarch64)

Build for Raspberry Pi 4/5 with OpenGL ES 3.0 support.

#### 1. Install Cross-Compilation Toolchain (on build machine)

```bash
sudo apt update
sudo apt install aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ zlib1g-dev:arm64
```

#### 2. Compile

```bash
# Raspberry Pi 5 (cortex-a76)
make platform=rpi64 -j$(nproc)

# or generic aarch64
make platform=aarch64 -j$(nproc)
```

The Makefile auto-tunes the CPU based on platform:
- **rpi5**: Cortex-A76 optimizations
- **rpi4-64**: Cortex-A72 optimizations
- **rpi64 (default)**: Cortex-A53 optimizations
- **aarch64**: Generic aarch64 (no CPU-specific tuning)

#### 3. Transfer to Raspberry Pi

```bash
scp supermodel_libretro_aarch64.so pi@raspberrypi:~/.config/retroarch/cores/supermodel_libretro.so
```

**Platform Features:**
- GLES 3.0 rendering via `glsym_es3`
- Legacy3D renderer excluded (GLES incompatible)
- Optimized for aarch64 ARM architecture
- Full force feedback support
- Synchronous 57.53Hz audio

**Performance Notes:** On Raspberry Pi, performance varies by game and model:
- **RPi 5:** Can handle most games at native resolution
- **RPi 4:** Older/simpler titles (VF3, Daytona) run well; may need scaling for demanding titles
- **RPi 3:** Limited performance; resolution scaling recommended

---

### Generic aarch64 (Standalone ARM64 Linux)

For generic ARM64 Linux systems (not Raspberry Pi):

```bash
make platform=aarch64 -j$(nproc)
```

Same as RPi64 but without CPU-specific tuning. Suitable for:
- ARM64 servers
- Generic ARM development boards
- Amazon Graviton instances
- Other aarch64 Linux systems

---

## 🎮 Performance Notes
For performance-heavy titles (e.g., Sega Rally 2 or Daytona USA 2), ensure you are running the core in Release mode. This core uses synchronous audio; if your CPU cannot maintain the full 57.53Hz emulation speed, you may experience audio stuttering.

On Raspberry Pi, performance depends on the specific model and game. Older titles (e.g., Virtua Fighter 3) run well on Pi 4/5, while demanding titles may require resolution scaling adjustments via RetroArch core options.
