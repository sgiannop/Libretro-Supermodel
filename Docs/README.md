# Supermodel (Sega Model 3) - Libretro Port (Modernized)

A modernized fork of the Sega Model 3 (Supermodel) Libretro core, optimized for modern Linux distributions and updated to C++17 standards.

## 🚀 Key Improvements
- **C++17 Migration:** Replaced legacy SDL-based threading and crosshair logic with native C++17 and Libretro-standard implementations.
- **Ubuntu 24.04 Compatibility:** Fixed header conflicts and link-time errors present in the original codebase.
- **Improved Input Mapping:** Full support for Analog/Digital gamepads and keyboard out of the box.
- **Native Performance:** Optimized for Ubuntu 24.04 (x86_64) hitting full native frame rates.
- **Libretro Portability:** Remapped configuration and asset paths to follow official Libretro standards.

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
sudo apt install build-essential libsdl2-dev libglew-dev libgl1-mesa-dev libglu1-mesa-dev zlib1g-dev
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