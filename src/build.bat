@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   SysForge Build Script v2.0
echo ============================================

set GCC=C:\msys64\mingw64\bin\g++.exe
set WINDRES=C:\msys64\mingw64\bin\windres.exe

REM Always build to sysforge.exe first, fall back to timestamped name
set OUTPUT=sysforge.exe

REM Try to delete old exe (works if not running)
if exist %OUTPUT% del /f /q %OUTPUT% >nul 2>&1

REM If still locked, build to a unique timestamped name instead
if exist %OUTPUT% (
    for /f "tokens=1-4 delims=:.," %%a in ("%TIME%") do (
        set TS=%%a%%b%%c%%d
    )
    set TS=!TS: =0!
    set OUTPUT=sysforge_!TS!.exe
    echo [INFO] sysforge.exe is locked. Building to !OUTPUT!
)

echo [STEP 1] Compiling resources...
"%WINDRES%" sysforge.rc -o sysforge_res.o
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Resource compile failed.
    pause & exit /b 1
)

echo [STEP 2] Compiling C++ (OpenMP + all libs)...
"%GCC%" -std=c++17 -fopenmp -O2 -o %OUTPUT% sysforge.cpp sysforge_res.o ^
    -lpdh -lpsapi -lcomctl32 -lgdi32 -luser32 -lkernel32 ^
    -ladvapi32 -lshell32 -lole32 -lcomdlg32 -lgdiplus -mwindows 2>&1

if %ERRORLEVEL% EQU 0 (
    echo ============================================
    echo  [SUCCESS] Built: %OUTPUT%
    echo  Double-click it and accept the UAC prompt.
    echo ============================================
    start "" %OUTPUT%
) else (
    echo [ERROR] Build failed. See errors above.
    pause
)
endlocal
