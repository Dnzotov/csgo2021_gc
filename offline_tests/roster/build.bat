@echo off
rem Offline tests of the srcds side of the backend-driven roster (RESEARCH_FINDINGS.md #55). Not part of the DLL build.
rem   controller_test.exe   decision table of RosterFeed::Controller (real server_roster.cpp), no network
rem   roster_e2e.exe        real backend_client + Poller + Controller + FakeRoster against a REAL backend on 127.0.0.1:18090
rem   launch_args_test.exe  -backend_ip/-backend_port of srcds: real launch_args.h + Poller + backend_client against a capturing
rem                         HTTP stand-in on 127.0.0.1:18093 (no real backend needed)
rem                         (api key test-api) and a UDP stand-in for the engine; see README.md
rem The real sources are copied to build\ first: they must not see the DLL's stdafx.h (protobuf), the stand-ins here replace it.
setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\csgo_gc
set OUT=%HERE%build
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%HERE%stdafx.h" "%OUT%\" >nul
copy /y "%HERE%config.h" "%OUT%\" >nul
copy /y "%HERE%funchook.h" "%OUT%\" >nul
copy /y "%HERE%roster_e2e.cpp" "%OUT%\" >nul
copy /y "%HERE%controller_test.cpp" "%OUT%\" >nul
copy /y "%HERE%launch_args_test.cpp" "%OUT%\" >nul
for %%F in (launch_args.h backend_client.h backend_client.cpp keyvalue.h keyvalue.cpp server_roster.h server_roster.cpp test_accept.h test_accept.cpp test_diag.h reservation_keepalive.h) do copy /y "%SRC%\%%F" "%OUT%\" >nul
call "%HERE%..\..\tools\msvc\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul 2>&1
cd /d "%OUT%"
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:controller_test.exe controller_test.cpp server_roster.cpp backend_client.cpp keyvalue.cpp ws2_32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:roster_e2e.exe roster_e2e.cpp backend_client.cpp server_roster.cpp test_accept.cpp keyvalue.cpp ws2_32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I. /Fe:launch_args_test.exe launch_args_test.cpp server_roster.cpp backend_client.cpp keyvalue.cpp ws2_32.lib || exit /b 1
echo.
echo built: %OUT%\controller_test.exe  %OUT%\roster_e2e.exe  %OUT%\launch_args_test.exe
