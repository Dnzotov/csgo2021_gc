@echo off
rem Equipment Sync offline test with the real protobuf / item schema / message framing (equipment_sync_test.cpp).
rem Needs a finished build_local.bat build (it links the objects of the DLL build and its vcpkg libraries), and runs with the game
rem folder as the working directory because ItemSchema reads csgo\scripts\items\items_game.txt (read only, nothing is written there).
rem   set GAMEDIR=...   to override "C:\Program Files (x86)\Steam\steamapps\common\csgo legacy"
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..
set NINJA=%ROOT%\Build\build_ninja
set OBJ=%NINJA%\csgo_gc\CMakeFiles\csgo_gc.dir
set LIBDIR=%NINJA%\vcpkg_installed\x86-windows-static\lib
if not defined GAMEDIR set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\csgo legacy"
set OUT=%HERE%build_equipment
if not exist "%OUT%" mkdir "%OUT%"
call "%ROOT%\tools\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
cd /d "%OUT%"
rem every object of the DLL except main.cpp (DLL export only) -- the precompiled header object has to be in, the generated protobuf
rem sources use it -- then the libraries: a response file
if exist link.rsp del link.rsp
for %%F in ("%OBJ%\*.obj") do (
  if /I not "%%~nxF"=="main.cpp.obj" echo "%%F">>link.rsp
)
for %%F in ("%OBJ%\generated\*.obj") do echo "%%F">>link.rsp
rem the DLL links libprotobuf-lite, not the full runtime
for %%F in ("%LIBDIR%\*.lib") do (
  if /I not "%%~nxF"=="libprotobuf.lib" if /I not "%%~nxF"=="libprotoc.lib" echo "%%F">>link.rsp
)
echo "%NINJA%\_deps\funchook-build\funchook.lib">>link.rsp
echo "%NINJA%\_deps\funchook-build\distorm.lib">>link.rsp
cl /nologo /std:c++20 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%\csgo_gc" /I"%NINJA%\csgo_gc\generated" /I"%ROOT%\steamworks\sdk\public" /I"%NINJA%\vcpkg_installed\x86-windows-static\include" /I"%NINJA%\_deps\funchook-src\include" /MT /Fe:equipment_sync_test.exe "%HERE%equipment_sync_test.cpp" @link.rsp ws2_32.lib bcrypt.lib user32.lib advapi32.lib shell32.lib ole32.lib psapi.lib || exit /b 1
echo.
pushd "%GAMEDIR%"
"%OUT%\equipment_sync_test.exe"
set RC=%ERRORLEVEL%
popd
exit /b %RC%
