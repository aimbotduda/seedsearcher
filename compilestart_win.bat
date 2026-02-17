@echo off
REM compilestart_win.bat - Build script for Windows (MinGW-w64 / MSYS2)
REM Run from the cubiomes directory after copying files there.

cd /d "%~dp0"

echo === Building cubiomes library ===

REM Try mingw32-make first (typical MSYS2/MinGW install), fall back to make
where mingw32-make >nul 2>nul
if %errorlevel%==0 (
    mingw32-make clean 2>nul
    mingw32-make native
) else (
    where make >nul 2>nul
    if %errorlevel%==0 (
        make clean 2>nul
        make native
    ) else (
        echo ERROR: Neither mingw32-make nor make found in PATH.
        echo Install MSYS2/MinGW-w64 and add its bin directory to PATH.
        exit /b 1
    )
)

if not exist libcubiomes.a (
    echo ERROR: libcubiomes.a was not built. Check for errors above.
    exit /b 1
)

echo.
echo === Building structure_finder.exe ===
gcc -O3 -march=native -ffast-math -o structure_finder.exe structure_finder_win.c libcubiomes.a -lm
if %errorlevel% neq 0 (
    echo ERROR: Failed to build structure_finder.exe
    exit /b 1
)

echo.
echo === Building groupfinder.exe ===
gcc -O3 -march=native -ffast-math -o findgroups\groupfinder.exe findgroups\groupfinder_win.c -lm
if %errorlevel% neq 0 (
    echo ERROR: Failed to build groupfinder.exe
    exit /b 1
)

echo.
echo === Build Complete ===
echo.
echo Run: structure_finder.exe
echo Run: findgroups\groupfinder.exe
