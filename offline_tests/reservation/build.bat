@echo off
rem Offline tests of the reservation handling (RESEARCH_FINDINGS.md #60, #61). Not part of the DLL build.
rem   keepalive_test.exe    the real reservation_keepalive.h against a model of the engine's reservation rules (#60)
rem   client_flow_test.exe  who may reserve an engine server: engine ConnectClient model + call graph of the real
rem                         gc_client.cpp / gc_server.cpp (#61, Training "Invalid user info")
setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\csgo_gc
set OUT=%HERE%build
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%HERE%keepalive_test.cpp" "%OUT%\" >nul
copy /y "%HERE%client_flow_test.cpp" "%OUT%\" >nul
copy /y "%SRC%\reservation_keepalive.h" "%OUT%\" >nul
copy /y "%SRC%\gc_client.cpp" "%OUT%\gc_client.cpp.src" >nul
copy /y "%SRC%\gc_server.cpp" "%OUT%\gc_server.cpp.src" >nul
call "%HERE%..\..\tools\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
cd /d "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:keepalive_test.exe keepalive_test.cpp || exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:client_flow_test.exe client_flow_test.cpp || exit /b 1
echo.
echo built: %OUT%\keepalive_test.exe, %OUT%\client_flow_test.exe
echo run:   keepalive_test.exe ^&^& client_flow_test.exe
