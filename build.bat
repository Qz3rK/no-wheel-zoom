@echo off
setlocal
cd /d "%~dp0"

echo === noWheelZoom build (MinGW-w64) ===

windres version.rc -O coff -o version.o
if errorlevel 1 goto fail

gcc -Os -s -mwindows -static -static-libgcc -ffunction-sections -fdata-sections -Wl,--gc-sections -o noWheelZoom.exe noWheelZoom.c version.o -luser32 -lshell32
if errorlevel 1 goto fail

del version.o

echo.
echo Build OK
objdump -p noWheelZoom.exe | findstr /i "DLL Name"
echo.
echo Expected imports: KERNEL32.dll, USER32.dll and the CRT only.
exit /b 0

:fail
echo.
echo Build FAILED
exit /b 1