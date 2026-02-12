# Supermodel (Sega Model 3) - Libretro Port (Modernized)

A modernized fork of the Sega Model 3 (Supermodel) Libretro core, optimized for modern Linux distributions and updated to C++17 standards.

## 🚀 Key Improvements
- **Native Libretro Audio:** Removed legacy SDL audio dependency in favor of native `audio_batch_cb` synchronization.
- **C++17 Migration:** Replaced legacy SDL-based threading and synchronization with native C++17 `std::mutex`, `std::lock_guard`, and atomic operations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase specifically for modern GCC versions.
- **Synchronous A/V Timing:** Coupled the emulator's internal hardware clock (57.53Hz) with Libretro's timing engine.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box with improved deadzone handling.
- **Libretro Portability:** Remapped configuration, NVRAM, and asset paths to follow official Libretro standards (`system` and `save` directories).
- **Windows Support:** Full cross-platform support with dedicated Windows build targets using MinGW and vendored dependencies.

## 📂 Required Assets
To run the core, you must place the emulator's configuration files in your RetroArch system directory. The core follows standard Libretro conventions and will look for assets in the following location:

* **Path:** `[RetroArch System Directory]/supermodel/Config/`
* **Required Files:** * `Games.xml`
    * `Supermodel.ini`

*Note: If these files are missing, the core will fail to initialize the game list and settings.*

## 🛠 Build Instructions (Ubuntu 24.04)

### 1. Install Dependencies
```bash
sudo apt update
sudo apt install build-essential libglew-dev libgl1-mesa-dev libglu1-mesa-dev zlib1g-dev
```

### 2. Compile for Linux (Native)
```bash
make -j$(nproc)
```

## 3. Building for Windows

You can build the core either natively on a Windows machine or via cross-compilation from a Linux system.

### Option A: Native Windows Build (Recommended)
To build the `.dll` directly on Windows, use the **MSYS2** environment with the **MinGW-w64** toolchain.

1.  **Install MSYS2**: Download and install from [msys2.org](https://www.msys2.org/).
2.  **Open the "MSYS2 MinGW 64-bit" terminal** (avoid using the default MSYS terminal).
3.  **Install Dependencies**:
    Run the following command to install the compiler, make, and zlib:
    ```bash
    pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-make mingw-w64-x86_64-zlib
    ```
4.  **Compile**:
    Navigate to the source directory and run:
    ```bash
    mingw32-make platform=win -j%NUMBER_OF_PROCESSORS%
    ```

---

### Option B: Cross-Compilation (from Ubuntu/Linux)
If you are developing on a Linux system (like Ubuntu 24.04) and want to generate a Windows `.dll`:

1.  **Install the MinGW-w64 toolchain**:
    ```bash
    sudo apt update
    sudo apt install binutils-mingw-w64-x86-64 g++-mingw-w64-x86-64 libz-mingw-w64-dev
    ```
2.  **Compile**:
    ```bash
    make platform=win -j$(nproc)
    ```

### 4. Install
Copy the resulting binary to your RetroArch cores directory:
* **Linux:** 
```bash
cp supermodel_libretro.so ~/.config/retroarch/cores/`
```
* **Windows:** 
```bash
# Copy supermodel_libretro.dll to your Windows RetroArch/cores/ directory
```

## 🎮 Performance Notes
For performance-heavy titles (e.g., Sega Rally 2 or Daytona USA 2), ensure you are running the core in Release mode. This core uses synchronous audio; if your CPU cannot maintain the full 57.53Hz emulation speed, you may experience audio stuttering.