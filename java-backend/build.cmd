@echo off
rem Builds target\matchmaking-backend.jar (tests included; pass -DskipTests to skip them).
rem Uses JAVA_HOME if set, otherwise the portable JDK in ..\tools\jdk21 (see README.md).
setlocal
cd /d "%~dp0"
if not defined JAVA_HOME if exist "%~dp0..\tools\jdk21\bin\java.exe" set "JAVA_HOME=%~dp0..\tools\jdk21"
if not defined JAVA_HOME (
    echo JAVA_HOME is not set and ..\tools\jdk21 does not exist. Install a JDK 21 and set JAVA_HOME.
    exit /b 1
)
call "%~dp0mvnw.cmd" -B package %*
exit /b %ERRORLEVEL%
