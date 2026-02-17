@echo off
REM compilestart_win.bat - Build script for Windows (MinGW-w64 / MSYS2)
REM Run from the cubiomes directory after copying files there.
REM
REM Usage:
REM   compilestart_win.bat          Build optimised for THIS machine (default)
REM   compilestart_win.bat dist     Build portable executable for distribution

cd /d "%~dp0"

REM ---- pick arch flags ------------------------------------------------
REM "dist" produces a portable binary safe for any x86-64 CPU (SSE2).
REM Default uses -march=native for maximum speed on the build machine.
if /i "%~1"=="dist" (
    echo [dist] Building portable executable for distribution
    set "ARCHFLAGS=-march=x86-64 -mtune=generic"
) else (
    echo [native] Building optimised for this machine
    set "ARCHFLAGS=-march=native"
)

set "CUBIOMES_SRC=noise.c biomes.c layers.c biomenoise.c generator.c finders.c util.c quadbase.c"

echo.
echo === Building structure_finder.exe (unity build) ===
gcc -O3 %ARCHFLAGS% -ffast-math -fwrapv -Wall -Wextra -o structure_finder.exe structure_finder_win.c %CUBIOMES_SRC% -lm
if %errorlevel% neq 0 (
    echo ERROR: Failed to build structure_finder.exe
    exit /b 1
)

echo.
echo === Building groupfinder.exe ===
gcc -O3 %ARCHFLAGS% -ffast-math -fwrapv -Wall -Wextra -o findgroups\groupfinder.exe findgroups\groupfinder_win.c -lm
if %errorlevel% neq 0 (
    echo ERROR: Failed to build groupfinder.exe
    exit /b 1
)

echo.
echo === Build Complete ===
echo.
echo Run: structure_finder.exe
echo Run: findgroups\groupfinder.exe
