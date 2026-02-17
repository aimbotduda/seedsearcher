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

Then copy all project files into the `cubiomes` directory (overwrite the makefile):

```
cp -r structure_finder.c structure_finder_win.c hutfinder.c makefile compilestart.sh compilestart_win.bat findgroups cubiomes/
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

Requires [MSYS2](https://www.msys2.org/) with MinGW-w64 toolchain installed.

### Install MSYS2 + MinGW-w64

1. Download and install MSYS2 from https://www.msys2.org/
2. Open the **MSYS2 UCRT64** terminal and install the toolchain:
   ```
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
   ```
3. Add MinGW to your system PATH (e.g. `C:\msys64\ucrt64\bin`)

### Build

From a **Command Prompt** or **PowerShell** (with MinGW in PATH), inside the `cubiomes` directory:

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
