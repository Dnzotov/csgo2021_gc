@echo off
rem Starts the matchmaking backend. Settings: config\application.properties (copy config\application-example.properties)
rem or environment variables (BACKEND_ADMIN_USERNAME, BACKEND_ADMIN_PASSWORD, BACKEND_API_KEY, SERVER_PORT ...).
rem Extra arguments are passed on, e.g.  run.cmd --server.port=9090
setlocal
cd /d "%~dp0"
if not defined JAVA_HOME if exist "%~dp0..\tools\jdk21\bin\java.exe" set "JAVA_HOME=%~dp0..\tools\jdk21"
if not defined JAVA_HOME (
    echo JAVA_HOME is not set and ..\tools\jdk21 does not exist. Install a JDK 21 and set JAVA_HOME.
    exit /b 1
)
if not exist target\matchmaking-backend.jar (
    echo target\matchmaking-backend.jar not found, building it first...
    call "%~dp0build.cmd" -DskipTests || exit /b 1
)
"%JAVA_HOME%\bin\java.exe" -jar target\matchmaking-backend.jar %*
exit /b %ERRORLEVEL%
