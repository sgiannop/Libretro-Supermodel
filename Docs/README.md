# Supermodel (Sega Model 3) - Libretro Port (Modernized)

A modernized fork of the Sega Model 3 (Supermodel) Libretro core, optimized for modern Linux distributions and updated to C++17 standards.

## 🚀 Key Improvements
- **Native Libretro Audio:** Removed legacy SDL audio dependency in favor of native `audio_batch_cb` synchronization.
- **C++17 Migration:** Replaced legacy SDL-based threading and synchronization with native C++17 `std::mutex`, `std::lock_guard`, and atomic operations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase specifically for modern GCC versions.
- **Synchronous A/V Timing:** Coupled the emulator's internal hardware clock (57.53Hz) with Libretro's timing engine.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box with improved deadzone handling.
- **Configurable Service & Test Buttons:** Service and Test buttons are now mappable through the RetroArch input configuration.
- **Force Feedback / Rumble:** Full force feedback support for steering wheel games via the Libretro rumble interface.
- **Widescreen Hack:** Optional widescreen mode exposed as a core option in the RetroArch UI.
- **Libretro Portability:** Remapped configuration, NVRAM, and asset paths to follow official Libretro standards (`system` and `save` directories).
- **No External GL Dependency:** GLEW replaced with `glsym` from libretro-common — no system GL extension library required on any platform.
- **Android Support:** Builds natively for Android (arm64, arm, x86_64) via the Android NDK with OpenGL ES 3.0.
- **Windows Support:** Full cross-platform support with dedicated Windows build targets using MinGW — no vendored prebuilt libraries required.

## 📂 Required Assets
To run the core, you must place the emulator's configuration files in your RetroArch system directory. The core follows standard Libretro conventions and will look for assets in the following location:

* **Path:** `[RetroArch System Directory]/supermodel/Config/`
* **Required Files:**
    * `Games.xml`
    * `Supermodel.ini`

*Note: If these files are missing, the core will fail to initialize the game list and settings.*

## 🛠 Build Instructions

### Linux (Native)

#### 1. Install Dependencies
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
    mingw32-make platform=win -j%NUMBER_OF_PROCESSORS%
    ```

#### Option B: Cross-Compilation (from Ubuntu/Linux)

1. **Install the MinGW-w64 toolchain**:
    ```bash
    sudo apt update
    sudo apt install binutils-mingw-w64-x86-64 g++-mingw-w64-x86-64 libz-mingw-w64-dev
    ```
2. **Compile**:
    ```bash
    make platform=win -j$(nproc)
    ```

Copy `supermodel_libretro.dll` to your RetroArch `cores/` directory.

---

### macOS (Intel & Apple Silicon)

This core now supports both Intel Macs (x86_64) and Apple Silicon Macs (arm64 M1/M2/M3+) through universal binary cross-compilation using osxcross.

#### Option A: Native macOS Build

If building on a macOS system with Xcode installed:

```bash
# Install dependencies via Homebrew
brew install zlib glew

# Compile
make platform=osx MACOS_ARCH=universal -j$(sysctl -n hw.ncpu)

# Install
cp supermodel_libretro.dylib ~/.config/retroarch/cores/
```

#### Option B: Cross-Compilation from Linux (Ubuntu/Debian)

This is the recommended approach for CI/CD and reproducible builds.

1. **Install osxcross toolchain**:
   ```bash
   # Clone osxcross repo
   git clone https://github.com/tpoechtrager/osxcross ~/dev/osxcross
   cd ~/dev/osxcross
   
   # Download macOS SDK (requires ~52MB, includes MacOSX12.0.sdk)
   # See osxcross documentation for SDK acquisition
   
   # Build toolchain
   ./build.sh
   ```

2. **Compile**:
   ```bash
   export OSXCROSS_ROOT=~/dev/osxcross
   export PATH="$OSXCROSS_ROOT/target/bin:$PATH"
   
   # Universal binary (x86_64 + arm64)
   make platform=osx MACOS_ARCH=universal \
     CC=o64-clang CXX=o64-clang++ LD=o64-clang++ -j$(nproc)
   
   # Or Intel only
   make platform=osx MACOS_ARCH=x86_64 \
     CC=o64-clang CXX=o64-clang++ LD=o64-clang++ -j$(nproc)
   
   # Or Apple Silicon only
   make platform=osx MACOS_ARCH=arm64 \
     CC=o64-clang CXX=o64-clang++ LD=o64-clang++ -j$(nproc)
   ```

3. **Transfer to macOS**:
   ```bash
   # Copy the .dylib to a macOS system
   scp supermodel_libretro.dylib user@mac:~/Downloads/
   
   # On the Mac, install to RetroArch
   cp ~/Downloads/supermodel_libretro.dylib \
     ~/.config/retroarch/cores/
   ```

**macOS Build Details:**
- **Deployment Target:** macOS 10.15 (Catalina) and newer
- **Architecture:** Universal binary (both x86_64 and arm64) for maximum compatibility
- **Renderer:** Modern OpenGL 3.2+ (no legacy fixed-pipeline)
- **Build Time:** ~90 seconds on 4 cores

**Note:** If building on macOS fails with GL header errors, ensure Xcode Command Line Tools are installed:
```bash
xcode-select --install
```

---

### Android

Requires the Android NDK. The build system checks `~/Android/Sdk/ndk/28.2.13676358` by default, or set `NDK_ROOT` to your NDK path.

```bash
# arm64 (aarch64 - default)
make platform=android -j$(nproc)

# 32-bit ARM (armv7)
make platform=android arch=arm -j$(nproc)

# x86_64
make platform=android arch=x86_64 -j$(nproc)

# x86 (32-bit)
make platform=android arch=x86 -j$(nproc)
```

Copy `supermodel_libretro.so` to your Android RetroArch cores directory.

---

### Raspberry Pi 4/5 (aarch64 / 64-bit)

This build targets Raspberry Pi 4 and 5 with OpenGL ES 3.0 support using the aarch64 cross-compiler.

#### 1. Install the Cross-Compilation Toolchain

```bash
sudo apt update
sudo apt install aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ zlib1g-dev:arm64
```

#### 2. Compile

```bash
# For Raspberry Pi 5 (Cortex-A76 optimization)
make platform=rpi5 -j$(nproc)

# For Raspberry Pi 4 (Cortex-A72 optimization)
make platform=rpi4-64 -j$(nproc)

# For generic aarch64 ARM (Cortex-A53, RPi 3B+ 64-bit if available)
make platform=rpi64 -j$(nproc)
```

#### 3. Install on Raspberry Pi

Copy the generated `supermodel_libretro.so` to your Raspberry Pi's RetroArch cores directory:

```bash
scp supermodel_libretro.so pi@raspberrypi:/home/pi/.config/retroarch/cores/
```

**Architecture Details:**
- **rpi5**: Cortex-A76 CPU tuning (Raspberry Pi 5)
- **rpi4-64**: Cortex-A72 CPU tuning (Raspberry Pi 4)
- **rpi64**: Cortex-A53 CPU tuning (generic aarch64)

**Platform Features:**
- Excludes Legacy3D renderer (OpenGL ES 3.0 only)
- Full GLES3 shading support via `glsym_es3`
- Optimized for aarch64 architecture

---

## 🎮 Performance Notes
For performance-heavy titles (e.g., Sega Rally 2 or Daytona USA 2), ensure you are running the core in Release mode. This core uses synchronous audio; if your CPU cannot maintain the full 57.53Hz emulation speed, you may experience audio stuttering.

On Raspberry Pi, performance depends on the specific model and game. Older titles (e.g., Virtua Fighter 3) run well on Pi 4/5, while demanding titles may require resolution scaling adjustments via RetroArch core options.
