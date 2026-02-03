# pc-control

Minimalistic Windows 11 command-line tool for remote PC power management via MQTT.

## Features

- Put PC to sleep mode
- Turn off monitor
- Online/offline status via MQTT LWT (Last Will and Testament)
- Automatic reconnection with exponential backoff
- Logs all commands with timestamps

## Dependencies

- [Paho MQTT C Client](https://github.com/eclipse/paho.mqtt.c)
- CMake 3.16+
- MinGW-w64

## Setup (Windows)

### 1. Install build tools via Chocolatey

Run in an **elevated (Administrator) shell**:
```cmd
choco install cmake mingw -y
```

### 2. Clone and build Paho MQTT C

```bash
# Clone the library (next to pc-control directory)
git clone https://github.com/eclipse/paho.mqtt.c.git ../paho.mqtt.c

# Configure with MinGW (adjust paths if MinGW is installed elsewhere)
cmake -B ../paho.mqtt.c/build -S ../paho.mqtt.c -G "MinGW Makefiles" ^
    -DPAHO_WITH_SSL=OFF ^
    -DPAHO_BUILD_STATIC=ON ^
    -DPAHO_BUILD_SAMPLES=OFF ^
    -DPAHO_ENABLE_TESTING=OFF ^
    -DCMAKE_C_COMPILER="C:/ProgramData/mingw64/mingw64/bin/gcc.exe" ^
    -DCMAKE_MAKE_PROGRAM="C:/ProgramData/mingw64/mingw64/bin/mingw32-make.exe"

# Build
mingw32-make -C ../paho.mqtt.c/build -j4

# Install to local prefix
cmake --install ../paho.mqtt.c/build --prefix ../paho.mqtt.c/install
```

### 3. Build pc-control

```bash
# Configure
cmake -B build -G "MinGW Makefiles" ^
    -DCMAKE_C_COMPILER="C:/ProgramData/mingw64/mingw64/bin/gcc.exe" ^
    -DCMAKE_MAKE_PROGRAM="C:/ProgramData/mingw64/mingw64/bin/mingw32-make.exe"

# Build
mingw32-make -C build
```

The build produces two standalone executables in `build/` (no DLLs required):

| Executable | Description |
|------------|-------------|
| `pc-control.exe` | Console app - shows output in terminal |
| `pc-control-hidden.exe` | GUI app - completely invisible, no window |

## Usage

```bash
pc-control.exe <broker_ip> <username> <password> [port] [hostname]
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| broker_ip | Yes | - | MQTT broker IP address |
| username | Yes | - | MQTT username |
| password | Yes | - | MQTT password |
| port | No | 1883 | MQTT broker port |
| hostname | No | System hostname | Device name used in topics |

Examples:
```bash
# Minimal (uses default port 1883 and system hostname)
pc-control.exe 192.168.1.100 myuser mypassword

# Custom port
pc-control.exe 192.168.1.100 myuser mypassword 1884

# Custom port and hostname
pc-control.exe 192.168.1.100 myuser mypassword 1883 gaming-pc

# Run completely hidden (no window at all)
pc-control-hidden.exe 192.168.1.100 myuser mypassword
```

## Autostart (Run at Login)

Use `pc-control-hidden.exe` for completely invisible background operation:

1. Create a shortcut to `pc-control-hidden.exe`
2. Right-click → Properties → Target:
   ```
   "C:\path\to\pc-control-hidden.exe" 192.168.1.100 myuser mypassword
   ```
3. Press `Win+R`, type `shell:startup`, press Enter
4. Move the shortcut to the Startup folder

The hidden version runs with no window, no flash, and no taskbar entry.

Check `pc-control.log` in the working directory to verify it's running.

## MQTT Topics

Topics include the hostname to support multiple PCs:

| Topic | Type | Description |
|-------|------|-------------|
| `pc-control/<hostname>/sleep` | Command | Put PC to sleep (send any message) |
| `pc-control/<hostname>/monitor-off` | Command | Turn off monitor (send any message) |
| `pc-control/<hostname>/status` | Status | `online` / `offline` (retained) |
| `pc-control/<hostname>/version` | Info | Version string, e.g. `1.0.0` (retained) |

### Status tracking (LWT)

The client publishes `online` to the status topic on connect, and the broker automatically publishes `offline` via Last Will and Testament when the client disconnects unexpectedly (crash, network failure, PC sleep).

This integrates seamlessly with:
- **Home Assistant** - MQTT binary sensor (see example below)
- **InfluxDB/Telegraf** - Subscribe to `pc-control/+/status` for all PCs
- **Grafana** - Visualize uptime/availability

**Home Assistant configuration.yaml:**
```yaml
mqtt:
  binary_sensor:
    - name: "Desktop PC"
      state_topic: "pc-control/desktop-pc/status"
      payload_on: "online"
      payload_off: "offline"
      device_class: connectivity
```

### Example commands

```bash
# Put desktop-pc to sleep
mosquitto_pub -h 192.168.1.100 -u myuser -P mypassword -t "pc-control/desktop-pc/sleep" -m "1"

# Turn off monitor
mosquitto_pub -h 192.168.1.100 -u myuser -P mypassword -t "pc-control/desktop-pc/monitor-off" -m "1"

# Check status (subscribe)
mosquitto_sub -h 192.168.1.100 -u myuser -P mypassword -t "pc-control/+/status" -v
```

Note: The hostname is sanitized (lowercased, spaces/dots replaced with dashes).

## Log

Commands are logged to `pc-control.log` in the working directory with timestamps:
```
[2026-02-01 16:04:23] Connected to MQTT broker
[2026-02-01 16:05:01] MONITOR_OFF command received - turning off monitor
[2026-02-01 16:10:15] SLEEP command received - entering sleep mode
```
## Authors

* Original creator: [Artur Brynka](https://github.com/FireHawken)

## Version history

| Version | Date       | Notes                                                                                                          |
|---------|------------|---------------------------------------------------------------------------------------------------------------|
| 1.1.0   | 02.03.2026 | Add new, separate "invisible" executable and statically link paho-mqtt, so only 1 file is required for distribution |
| 1.0.1   | 02.02.2026 | Add logging to file, improved Home Assistant support, status topic (`online`/`offline`) via LWT, monitor off command. |
| 1.0.0   | 01.02.2026 | Initial release                          |