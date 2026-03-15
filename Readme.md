# hearing_phase4_poc (Phase 4) — Build & Run Guide (Windows + Visual Studio)

This README explains how to:
1) Install the required tools (Visual Studio)
2) Build the project
3) Run different test cases (multiband static, multiband compress, broadband, export bands)
4) Fix common input WAV errors (must be mono 16-bit PCM)

---

## 0) Project assumptions

- You are on Windows 10/11
- Project root contains CMakeLists.txt and `src/main.cpp`
- You already updated `main.cpp` to the version that supports:
  - `--mode static|compress|broadband`
  - `--export_bands --stage in|on --export_root ...`

---

## 1) Install Visual Studio (Required)

### Install Visual Studio Community
Download and install **Visual Studio Community**.

During installation, select workloads:

✅ **Desktop development with C++**

Also make sure these are selected (usually auto-selected):
- MSVC v143 (or latest) C++ build tools
- Windows 10/11 SDK
- CMake tools for Windows (recommended)

> After install, restart PC if prompted.

---

## 2) Open the correct terminal (Important)

Use one of these (either is fine):
- **“x64 Native Tools Command Prompt for VS”**
- **“Developer PowerShell for VS”**

This ensures the correct compiler is on PATH.

---

### 2.1) If x86 then turn it into x64

$vsdev = "C:\Visual Studio\Product\Community 18\Common7\Tools\VsDevCmd.bat"
cmd /s /c "`"$vsdev`" -arch=x64 -host_arch=x64 && set VSCMD && where cl && where link && where rc && powershell"

### 2.2)Confirm vcpkg works and install only what we need
Go to your vcpkg folder:

cd "C:\vcpkg\vcpkg"
.\vcpkg version


## 3) Build the project (CMake)

From the project root:

Use VS CMake instead. Run:

$cmake = "C:\Visual Studio\Product\Community 18\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --version

& $cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Optional (Cache Clearing) - Remove-Item -Recurse -Force build

& $cmake --build build


# Test Cases

`export multibands`

.\build\hearing_phase4_poc.exe `
 --in "test_audio\sweep_100_10k.wav" `
 --export_bands `
 --stage in `
 --export_root "results"

`Patch Broadband vs Multiband`

PowerShell terminal runners (copy-paste)

1) `Multiband (static)`

.\build\hearing_phase4_poc.exe `
  --mode static `
  --in "test_audio\sweep_100_10k.wav" `
  --out_off "results\sweep_off.wav" `
  --out_on  "results\sweep_multiband_static.wav" `
  --loss "0,10,20,30,40,50"

2) `Multiband (compress)`

.\build\hearing_phase4_poc.exe `
  --mode compress `
  --in "test_audio\sweep_100_10k.wav" `
  --out_off "results\sweep_off.wav" `
  --out_on  "results\sweep_multiband_compress.wav" `
  --loss "0,10,20,30,40,50"

3) `Broadband (single gain from average hearing loss)`

.\build\hearing_phase4_poc.exe `
  --mode broadband `
  --in "test_audio\sweep_100_10k.wav" `
  --out_off "results\sweep_off.wav" `
  --out_on  "results\sweep_broadband.wav" `
  --loss "0,10,20,30,40,50"

4) `Export 6 raw split bands (for checking crossover)`

.\build\hearing_phase4_poc.exe `
  --in "test_audio\sweep_100_10k.wav" `
  --export_bands `
  --stage in `
  --export_root "results"

5) `Export 6 processed bands (after gain/compress)`

.\build\hearing_phase4_poc.exe `
  --in "test_audio\sweep_100_10k.wav" `
  --mode static `
  --loss "0,10,20,30,40,50" `
  --export_bands `
  --stage on `
  --export_root "results"


## New - Run only realtime multiband
cmake -S . -B build -G Ninja `
-DCMAKE_TOOLCHAIN_FILE="D:\0.Installations\vcpkg\vcpkg\scripts\buildsystems\vcpkg.cmake" `
-DVCPKG_TARGET_TRIPLET=x64-windows `
-DCMAKE_BUILD_TYPE=Release

cmake --build build --target realtime_multiband_stream
  






