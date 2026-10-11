@echo off
rem Build PomodoroNative.exe (pure Win32, no C runtime) with MinGW gcc
cd /d "%~dp0"
if not exist emoji_data.h powershell -NoProfile -ExecutionPolicy Bypass -File gen_emoji.ps1
windres -O coff -i pomodoro.rc -o pomodoro_res.o || (pause & exit /b 1)
gcc -Os -s -std=c11 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns ^
  -fno-asynchronous-unwind-tables -fno-ident -fno-stack-check -mno-stack-arg-probe ^
  -ffunction-sections -fdata-sections -nostdlib -nostartfiles -e start ^
  -Wl,--subsystem,windows -Wl,--gc-sections -Wl,--build-id=none ^
  pomodoro.c pomodoro_res.o -lkernel32 -luser32 -lgdi32 -ldwmapi ^
  -o ..\PomodoroNative.exe || (pause & exit /b 1)
del pomodoro_res.o
echo Build OK: %~dp0..\PomodoroNative.exe
