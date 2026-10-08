@echo off
rem Offline test of the skin sync payloads (research/backend_skin_sync_design.md). Not part of the DLL build.
rem   skinsync_test.exe   client snapshot -> JSON payload -> parsed by the server side parser (real skin_snapshot.cpp / backend_client.cpp)
rem The real sources are copied to build\ first: they must not see the DLL's stdafx.h (protobuf), the stand-ins of ..\roster replace it.
setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\csgo_gc
set STANDINS=%HERE%..\roster
set OUT=%HERE%build
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%STANDINS%\stdafx.h" "%OUT%\" >nul
copy /y "%STANDINS%\config.h" "%OUT%\" >nul
copy /y "%STANDINS%\funchook.h" "%OUT%\" >nul
copy /y "%HERE%skinsync_test.cpp" "%OUT%\" >nul
for %%F in (skin_snapshot.h skin_snapshot.cpp equipment_snapshot.h equipment_snapshot.cpp backend_client.h backend_client.cpp keyvalue.h keyvalue.cpp launch_args.h) do copy /y "%SRC%\%%F" "%OUT%\" >nul
call "%HERE%..\..\tools\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
cd /d "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:skinsync_test.exe skinsync_test.cpp skin_snapshot.cpp equipment_snapshot.cpp backend_client.cpp keyvalue.cpp ws2_32.lib || exit /b 1
echo.
.\skinsync_test.exe
exit /b %ERRORLEVEL%
