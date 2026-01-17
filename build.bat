@echo off
REM Sol IDE - Build Script for Windows
setlocal enabledelayedexpansion

echo ======================================
echo   Sol IDE Build System (Windows)
echo ======================================
echo.

REM Default settings
set BUILD_TYPE=Release
set CLEAN=0
set TESTS=0
set VERBOSE=0
set INSTALL=0
set JOBS=%NUMBER_OF_PROCESSORS%

REM Parse arguments
:parse_args
if "%~1"=="" goto :done_args
if /i "%~1"=="-d" set BUILD_TYPE=Debug& goto :next_arg
if /i "%~1"=="--debug" set BUILD_TYPE=Debug& goto :next_arg
if /i "%~1"=="-r" set BUILD_TYPE=Release& goto :next_arg
if /i "%~1"=="--release" set BUILD_TYPE=Release& goto :next_arg
if /i "%~1"=="-c" set CLEAN=1& goto :next_arg
if /i "%~1"=="--clean" set CLEAN=1& goto :next_arg
if /i "%~1"=="-t" set TESTS=1& goto :next_arg
if /i "%~1"=="--tests" set TESTS=1& goto :next_arg
if /i "%~1"=="-v" set VERBOSE=1& goto :next_arg
if /i "%~1"=="--verbose" set VERBOSE=1& goto :next_arg
if /i "%~1"=="-i" set INSTALL=1& goto :next_arg
if /i "%~1"=="--install" set INSTALL=1& goto :next_arg
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-j" (
    set JOBS=%~2
    shift
    goto :next_arg
)
if /i "%~1"=="--jobs" (
    set JOBS=%~2
    shift
    goto :next_arg
)
echo Unknown option: %~1
goto :usage

:next_arg
shift
goto :parse_args

:usage
echo Usage: build.bat [options]
echo.
echo Options:
echo   -d, --debug       Build in debug mode
echo   -r, --release     Build in release mode (default)
echo   -c, --clean       Clean build directory before building
echo   -t, --tests       Build and run tests
echo   -v, --verbose     Verbose build output
echo   -i, --install     Install after building
echo   -j, --jobs N      Number of parallel jobs (default: %NUMBER_OF_PROCESSORS%)
echo   -h, --help        Show this help message
exit /b 0

:done_args

REM Detect Visual Studio
echo Detecting Visual Studio...

set VS_FOUND=0
set CMAKE_GENERATOR=

REM Try VS 2022
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    set VS_FOUND=1
    set CMAKE_GENERATOR=-G "Visual Studio 17 2022" -A x64
    echo   Found Visual Studio 2022
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    set VS_FOUND=1
    set CMAKE_GENERATOR=-G "Visual Studio 17 2022" -A x64
    echo   Found Visual Studio 2022 Professional
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    set VS_FOUND=1
    set CMAKE_GENERATOR=-G "Visual Studio 17 2022" -A x64
    echo   Found Visual Studio 2022 Enterprise
)

REM Try VS 2019
if %VS_FOUND%==0 (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
        set VS_FOUND=1
        set CMAKE_GENERATOR=-G "Visual Studio 16 2019" -A x64
        echo   Found Visual Studio 2019
    )
)

REM Try Ninja with MSVC
if %VS_FOUND%==0 (
    where ninja >nul 2>&1
    if !errorlevel!==0 (
        where cl >nul 2>&1
        if !errorlevel!==0 (
            set VS_FOUND=1
            set CMAKE_GENERATOR=-G Ninja
            echo   Found Ninja with MSVC
        )
    )
)

if %VS_FOUND%==0 (
    echo ERROR: Visual Studio not found. Please install Visual Studio 2019 or 2022.
    exit /b 1
)

REM Check for CMake
echo.
echo Checking for CMake...
where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: CMake not found. Please install CMake 3.16 or later.
    exit /b 1
)
for /f "tokens=3" %%v in ('cmake --version ^| findstr /i version') do set CMAKE_VERSION=%%v
echo   CMake version: %CMAKE_VERSION%

REM Build directory
set BUILD_DIR=build\windows-x64-%BUILD_TYPE%

echo.
echo Build configuration:
echo   Build type: %BUILD_TYPE%
echo   Build directory: %BUILD_DIR%
echo   Parallel jobs: %JOBS%
echo.

REM Clean if requested
if %CLEAN%==1 (
    echo Cleaning build directory...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo   Clean complete
)

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM Configure CMake
echo Configuring with CMake...
set CMAKE_ARGS=-B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_GENERATOR%

if %TESTS%==1 set CMAKE_ARGS=%CMAKE_ARGS% -DSOL_BUILD_TESTS=ON

cmake %CMAKE_ARGS% .

if %errorlevel% neq 0 (
    echo ERROR: CMake configuration failed
    exit /b 1
)
echo   Configuration complete

REM Build
echo.
echo Building Sol IDE...
set BUILD_ARGS=--build "%BUILD_DIR%" --config %BUILD_TYPE% -j %JOBS%

if %VERBOSE%==1 set BUILD_ARGS=%BUILD_ARGS% --verbose

cmake %BUILD_ARGS%

if %errorlevel% neq 0 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo ======================================
echo   Build Successful!
echo ======================================
echo.
echo Binary location: %BUILD_DIR%\bin\%BUILD_TYPE%\sol.exe
echo.
echo To run Sol:
echo   %BUILD_DIR%\bin\%BUILD_TYPE%\sol.exe [file/directory]
echo.

REM Run tests if requested
if %TESTS%==1 (
    echo Running tests...
    pushd "%BUILD_DIR%"
    ctest --config %BUILD_TYPE% --output-on-failure
    popd
)

REM Install if requested
if %INSTALL%==1 (
    echo Installing...
    cmake --install "%BUILD_DIR%" --config %BUILD_TYPE%
)

endlocal
