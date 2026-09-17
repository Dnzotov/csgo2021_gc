@echo off
REM Local portable build for csgo2021_gc. Uses the project-local tools\
REM (msvc, cmake, ninja, vcpkg) -- nothing global is touched.
setlocal
set ROOT=%~dp0
set TOOLS=%ROOT%tools
call "%TOOLS%\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if errorlevel 1 exit /b 1
set PATH=%TOOLS%\cmake\bin;%TOOLS%\ninja;%PATH%
cd /d "%ROOT%"
cmake -G Ninja -S . -B Build\build_ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe ^
  -DCMAKE_TOOLCHAIN_FILE=%TOOLS%\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET:STRING=x86-windows-static ^
  -DOUTDIR=%ROOT%Build\release
if errorlevel 1 exit /b 1
cmake --build Build\build_ninja --target csgo_gc -v
if errorlevel 1 exit /b 1
cmake --build Build\build_ninja --target csgo -v
if errorlevel 1 exit /b 1
