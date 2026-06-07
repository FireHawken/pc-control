# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

pc-control is a minimalistic Windows 11 command-line tool that listens to MQTT commands to put the PC to sleep or turn off the monitor for energy saving.

## Build Commands

```bash
# Configure (MinGW installed via choco at default location)
cmake -B build -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER="C:/ProgramData/mingw64/mingw64/bin/gcc.exe" ^
    -DCMAKE_MAKE_PROGRAM="C:/ProgramData/mingw64/mingw64/bin/mingw32-make.exe"

# Build
mingw32-make -C build

# Clean
mingw32-make -C build clean
```

Requires: CMake 3.16+, MinGW-w64 (via `choco install mingw`), Paho MQTT C library built and installed at `../paho.mqtt.c/install/`.

See README.md for full setup instructions including Paho MQTT C build steps.

## Usage

```bash
# Console version (shows output)
pc-control.exe [--hide] <broker_ip> <username> <password> [port] [hostname]

# Hidden version (no window at all)
pc-control-hidden.exe <broker_ip> <username> <password> [port] [hostname]
```

- `port` defaults to 1883
- `hostname` defaults to system hostname (used in topics and client ID)

MQTT topics (hostname-based for multi-PC support):
- `pc-control/<hostname>/sleep` - Put PC to sleep (command payload: `1`, `true`, `on`, or `yes`; retained messages are ignored)
- `pc-control/<hostname>/monitor-off` - Turn off monitor (command payload: `1`, `true`, `on`, or `yes`; retained messages are ignored)
- `pc-control/<hostname>/status` - `online`/`offline` (retained, LWT)
- `pc-control/<hostname>/version` - Version string (retained)

Logs are written to `pc-control.log` in the working directory.

## Architecture

Two executables built from `src/main.c`:
- `pc-control.exe` - Console subsystem, shows terminal output
- `pc-control-hidden.exe` - GUI subsystem with `mainCRTStartup` entry point, no window

Both use the same source code. The hidden version is built with `-mwindows -Wl,-e,mainCRTStartup` flags, which creates a GUI app that can still use regular `main()` function.

Features:
- MQTT connection and message handling via Paho MQTT C
- LWT (Last Will and Testament) for offline detection
- Birth messages: publishes `online` status and version on connect
- Automatic reconnection with exponential backoff (1s to 30s)
- Hostname sanitization (lowercase, special chars handled)
- Windows API calls: `SetSuspendState()` for sleep, `SendMessageTimeout(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2, ...)` for monitor off
- Simple file-based logging with timestamps

## Version

Update `VERSION` and version number components in `src/version.h` when releasing.

## Key Windows APIs

- `SetSuspendState(FALSE, FALSE, FALSE)` - Sleep mode (requires linking `powrprof.lib`)
- `SendMessageTimeout` with `SC_MONITORPOWER` - Monitor power control with timeout

## Build Notes

The hidden version uses a technique from [Switchy](https://github.com/erryox/Switchy):
- Link with `-mwindows` (GUI subsystem)
- Set entry point to `mainCRTStartup` via `-Wl,-e,mainCRTStartup`
- This allows using regular `main()` while having no console window
