@echo off
setlocal EnableDelayedExpansion

set "SOURCE_DIR=%~dp0source"
set "BUILD_DIR=%~dp0build"
set "OUT_DIR=%~dp0Releases"

:: --- Argument parsing ----------------------------------------------------
:: --no-pause   skip the trailing 'pause' (use when running from a script/agent)
:: --debug-only / --release-only restrict the build to one config
set "NO_PAUSE=0"
set "BUILD_RELEASE=1"
set "BUILD_DEBUG=1"
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-pause"     ( set "NO_PAUSE=1"      & shift & goto parse_args )
if /I "%~1"=="--debug-only"   ( set "BUILD_RELEASE=0" & shift & goto parse_args )
if /I "%~1"=="--release-only" ( set "BUILD_DEBUG=0"   & shift & goto parse_args )
echo [WARN] Unknown argument: %~1
shift
goto parse_args
:args_done

echo.
echo ============================================================
echo  LumaCore Build (ALWAYS CLEAN)
echo  Source  : %SOURCE_DIR%
echo  Build   : %BUILD_DIR%
echo  Output  : %OUT_DIR%
echo  Release : %BUILD_RELEASE%   Debug: %BUILD_DEBUG%
echo ============================================================
echo.

:: --- ALWAYS delete build directory to prevent stale cache issues ---
if exist "%BUILD_DIR%" (
    echo [STEP] Deleting old build directory...
    rmdir /S /Q "%BUILD_DIR%"
    if exist "%BUILD_DIR%" (
        echo [ERROR] Failed to delete %BUILD_DIR% (file in use?)
        if "%NO_PAUSE%"=="0" pause
        exit /b 1
    )
)

:: --- Locate cmake: try PATH first, then the VS 2022 default install ---
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if !errorlevel! neq 0 (
    set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        echo [ERROR] cmake not found. Add cmake to PATH or install VS 2022.
        if "%NO_PAUSE%"=="0" pause
        exit /b 1
    )
    echo [INFO] Using cmake from VS install: !CMAKE_EXE!
)

:: --- Always use Visual Studio 17 2022 with MSBuild (x64) ---
:: Ninja requires a pre-configured MSVC environment (vcvars) which is not
:: guaranteed in all build environments. The VS generator handles MSVC
:: discovery automatically and is more robust for CI and local builds.
set "GENERATOR=Visual Studio 17 2022"
set "GEN_ARGS=-A x64"
echo [INFO] Using Visual Studio 17 2022 generator (MSBuild/MSVC x64)

:: --- Configure ---
echo [STEP] Configuring...
mkdir "%BUILD_DIR%" 2>nul
"!CMAKE_EXE!" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "!GENERATOR!" !GEN_ARGS!
if !errorlevel! neq 0 (
    echo [ERROR] Configure failed.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

:: --- Build Release and Debug ---
set "BUILD_FAILED=0"

if "%BUILD_RELEASE%"=="1" (
    echo.
    echo [STEP] Building Release...
    "!CMAKE_EXE!" --build "%BUILD_DIR%" --config Release --parallel
    if !errorlevel! neq 0 (
        echo [WARN] Release build failed.
        set "BUILD_FAILED=1"
    )
)

if "%BUILD_DEBUG%"=="1" (
    echo.
    echo [STEP] Building Debug...
    "!CMAKE_EXE!" --build "%BUILD_DIR%" --config Debug --parallel
    if !errorlevel! neq 0 (
        echo [WARN] Debug build failed.
        set "BUILD_FAILED=1"
    )
)

:: --- Copy DLLs to Releases\<Config>\ ---
echo.
echo [STEP] Copying DLLs to %OUT_DIR%...

if "%BUILD_RELEASE%"=="1" (
    set "RELEASE_BUILD_DIR=%BUILD_DIR%\Release"
    :: VS generator puts output in <build>\Release\; also check <build>\src\Release\ variants
    if not exist "!RELEASE_BUILD_DIR!\LumaCore.dll" (
        set "RELEASE_BUILD_DIR=%BUILD_DIR%\LumaCore\Release"
    )
    if exist "!RELEASE_BUILD_DIR!\LumaCore.dll" (
        mkdir "%OUT_DIR%\Release" 2>nul
        copy /Y "!RELEASE_BUILD_DIR!\LumaCore.dll" "%OUT_DIR%\Release\" >nul
        echo [OK] Copied LumaCore.dll (Release)
        if exist "!RELEASE_BUILD_DIR!\dwmapi.dll" (
            copy /Y "!RELEASE_BUILD_DIR!\dwmapi.dll" "%OUT_DIR%\Release\" >nul
            echo [OK] Copied dwmapi.dll (Release)
        )
        echo [OK] Release DLLs copied to %OUT_DIR%\Release
    ) else (
        echo [WARN] Release LumaCore.dll not found. Searched:
        echo        %BUILD_DIR%\Release\
        echo        %BUILD_DIR%\LumaCore\Release\
        dir /s /b "%BUILD_DIR%\*.dll" 2>nul || echo        (no DLLs found in build tree)
        if "%BUILD_FAILED%"=="0" set "BUILD_FAILED=1"
    )
)

if "%BUILD_DEBUG%"=="1" (
    set "DEBUG_BUILD_DIR=%BUILD_DIR%\Debug"
    if not exist "!DEBUG_BUILD_DIR!\LumaCore.dll" (
        set "DEBUG_BUILD_DIR=%BUILD_DIR%\LumaCore\Debug"
    )
    if exist "!DEBUG_BUILD_DIR!\LumaCore.dll" (
        mkdir "%OUT_DIR%\Debug" 2>nul
        copy /Y "!DEBUG_BUILD_DIR!\LumaCore.dll" "%OUT_DIR%\Debug\" >nul
        echo [OK] Copied LumaCore.dll (Debug)
        if exist "!DEBUG_BUILD_DIR!\dwmapi.dll" (
            copy /Y "!DEBUG_BUILD_DIR!\dwmapi.dll" "%OUT_DIR%\Debug\" >nul
            echo [OK] Copied dwmapi.dll (Debug)
        )
        echo [OK] Debug DLLs copied to %OUT_DIR%\Debug
    ) else (
        echo [WARN] Debug LumaCore.dll not found. Searched:
        echo        %BUILD_DIR%\Debug\
        echo        %BUILD_DIR%\LumaCore\Debug\
        dir /s /b "%BUILD_DIR%\*.dll" 2>nul || echo        (no DLLs found in build tree)
        if "%BUILD_FAILED%"=="0" set "BUILD_FAILED=1"
    )
)

echo.
echo ============================================================
echo  Done. DLLs are in:
if "%BUILD_RELEASE%"=="1" echo    %OUT_DIR%\Release
if "%BUILD_DEBUG%"=="1"   echo    %OUT_DIR%\Debug
echo ============================================================
echo.

if "%NO_PAUSE%"=="0" pause
endlocal
exit /b %BUILD_FAILED%
