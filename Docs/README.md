# Supermodel (Sega Model 3) - Libretro Port (Modernized)

A modernized fork of the Sega Model 3 (Supermodel) Libretro core, optimized for modern Linux distributions and updated to C++17 standards.

## 🚀 Key Improvements
- **Native Libretro Audio:** Removed legacy SDL audio dependency in favor of native `audio_batch_cb` synchronization.
- **C++17 Migration:** Replaced legacy SDL-based threading and synchronization with native C++17 `std::mutex`, `std::lock_guard`, and atomic operations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase specifically for modern GCC versions.
- **Synchronous A/V Timing:** Coupled the emulator's internal hardware clock (57.53Hz) with Libretro's timing engine.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box with improved deadzone handling.
- **Libretro Portability:** Remapped configuration, NVRAM, and asset paths to follow official Libretro standards (`system` and `save` directories).

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

### 2. Compile the project
```bash
make -j$(nproc)
```

### 3. Install
Copy the resulting supermodel_libretro.so to your RetroArch cores directory:

```bash
cp supermodel_libretro.so ~/.config/retroarch/cores/
```
## 🎮 Performance Notes
For performance-heavy titles (e.g., Sega Rally 2 or Daytona USA 2), ensure you are running the core in Release mode. This core uses synchronous audio; if your CPU cannot maintain the full 57.53Hz emulation speed, you may experience audio stuttering.