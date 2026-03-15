# Build and Run Guidelines

This document explains how to configure, build, and run the `realtime_multiband_stream` project on both Windows and Linux. 

## Prerequisites
- **CMake** (version 3.15 or higher)
- **A Build System** (like **Ninja** or Make)
- **A C++17 compatible compiler** (MSVC, GCC, Clang)
- **PortAudio Library**

---

## 🐧 Linux 

### 1. Install Dependencies
Install the required PortAudio development headers and build tools via your distribution's package manager.

**Ubuntu / Debian-based:**
```bash
sudo apt-get update
sudo apt-get install portaudio19-dev ninja-build build-essential cmake
```

**Arch / Manjaro-based:**
```bash
sudo pacman -S portaudio ninja base-devel cmake
```

### 2. Configure and Build
From the root of the project repository, configure the project using CMake and build it using Ninja.

```bash
# 1. Configure the CMake project
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 2. Build the specific target executable
cmake --build build --target realtime_multiband_stream
```

### 3. Run the Project
After a successful build, the executable will be located inside the `build` directory.

```bash
./build/realtime_multiband_stream
```

---

## 🪟 Windows

### 1. Install Dependencies 
We recommend using **vcpkg** to install PortAudio on Windows. Make sure y ou have Visual Studio (with Desktop development with C++) installed.

1. Open **Developer PowerShell for VS**.
2. Install `portaudio` using vcpkg:
```powershell
vcpkg install portaudio:x64-windows
```

### 2. Configure and Build
Using the same Developer PowerShell, configure the project. You must point CMake to your local `vcpkg.cmake` toolchain file.

```powershell
# 1. Configure the CMake project 
# IMPORTANT: Replace `<path_to_vcpkg>` with the actual path to your vcpkg installation (e.g., C:\vcpkg)
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="<path_to_vcpkg>\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DCMAKE_BUILD_TYPE=Release

# 2. Build the specific target executable
cmake --build build --target realtime_multiband_stream
```

### 3. Run the Project
After a successful build, run the executable out of the `build` folder.

```powershell
.\build\realtime_multiband_stream.exe
```
