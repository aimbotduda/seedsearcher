# seedsearcher

Scans Minecraft seeds for structures using [cubiomes](https://github.com/Cubitect/cubiomes), then finds groups of nearby structures.

## Prerequisites

- **GCC** (via system package manager, MinGW-w64, or MSYS2)
- **GNU Make** (`make` on Linux/macOS, `mingw32-make` or `make` on Windows)
- **Git** (to clone cubiomes)

## Setup

Clone cubiomes into this directory:

```
git clone https://github.com/Cubitect/cubiomes
```

Then copy all project files into the `cubiomes` directory (overwrite the makefile).

**Linux / macOS (bash):**

```bash
cp -r structure_finder.c structure_finder_win.c hutfinder.c makefile compilestart.sh compilestart_win.bat findgroups cubiomes/
```

**Windows (PowerShell):**

```powershell
Copy-Item structure_finder.c, structure_finder_win.c, hutfinder.c, makefile, compilestart.sh, compilestart_win.bat -Destination cubiomes/
Copy-Item findgroups -Destination cubiomes/ -Recurse
```

Then `cd cubiomes` for all build steps below.

---

## Linux / macOS

### Build

```bash
./compilestart.sh
```

This builds the cubiomes library and compiles the executables.

### Run

```bash
./structure_finder
```

To find groups of structures from the output:

```bash
cd findgroups
make
./groupfinder
```

---

## Windows

Requires a MinGW-w64 toolchain (GCC + GNU Make). Pick **any one** of the options below.

### Option A: w64devkit (easiest -- portable, no install)

1. Download the latest release from https://github.com/skeeto/w64devkit/releases
2. Extract the zip anywhere (e.g. `C:\w64devkit`)
3. Run `w64devkit.exe` -- this opens a shell with `gcc`, `make`, and `ar` ready to use
4. `cd` to the `cubiomes` directory and proceed to [Build](#build) below

### Option B / C / D: WinLibs, MSYS2, or Chocolatey

Pick one of:

- **WinLibs**: Download a release from https://winlibs.com/ (pick the UCRT runtime), extract it
- **MSYS2**: Install from https://www.msys2.org/, then in the **MSYS2 UCRT64** terminal run:
  ```
  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
  ```
- **Chocolatey**: `choco install mingw` (PATH is added automatically)

Then make sure MinGW is in your system PATH. Open PowerShell and check:

```powershell
gcc --version
```

If that prints "not recognized", add it to PATH:

1. Find the `bin` folder containing `gcc.exe` (common locations: `C:\msys64\ucrt64\bin`, `C:\mingw64\bin`)
2. Press `Win + R`, type `sysdm.cpl`, hit Enter
3. Go to **Advanced** tab > **Environment Variables**
4. Under **System variables**, select **Path**, click **Edit**
5. Click **New** and paste the path to the `bin` folder
6. Click OK on all dialogs and reopen PowerShell

### Build

From a **Command Prompt**, **PowerShell**, or **w64devkit shell** (with `gcc` available), inside the `cubiomes` directory:

```
compilestart_win.bat
```

This builds the cubiomes library and compiles the Windows executables (`structure_finder.exe` and `findgroups\groupfinder.exe`).

### Run

```
structure_finder.exe
```

To find groups of structures from the output:

```
findgroups\groupfinder.exe
```

---

## How It Works

1. **structure_finder** scans the entire Minecraft world (all regions) for selected structure types and writes their coordinates to files in a temp directory.
2. **groupfinder** reads those coordinate files and finds clusters of 3 or 4 structures within a specified radius. It auto-detects system RAM and optimizes its strategy accordingly.

## Files

| File | Description |
|------|-------------|
| `structure_finder.c` | Structure scanner (Linux/macOS) |
| `structure_finder_win.c` | Structure scanner (Windows) |
| `hutfinder.c` | Legacy hut/monument scanner |
| `findgroups/groupfinder.c` | Group finder (Linux/macOS) |
| `findgroups/groupfinder_win.c` | Group finder (Windows) |
| `compilestart.sh` | Build script (Linux/macOS) |
| `compilestart_win.bat` | Build script (Windows) |
| `makefile` | Makefile for cubiomes library + executables |
