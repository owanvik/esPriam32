# Copilot Instructions for esPriam32

## Project Overview

esPriam32 is a native ESP-IDF firmware for ESP32-C6 that creates a BLE-to-WiFi bridge for the Cybex E-Priam stroller. It provides:
- Web interface for control
- MQTT integration with Home Assistant auto-discovery
- OTA firmware updates

## Technology Stack

- **Framework**: ESP-IDF 5.5.1 (NOT Arduino, NOT ESPHome)
- **Build System**: PlatformIO
- **BLE Stack**: Apache NimBLE (GATT client)
- **Target Hardware**: Seeed Studio XIAO ESP32-C6

## Project Structure

```
esPriam32/
├── src/main.c          # All firmware code (~1300 lines)
├── platformio.ini      # PlatformIO configuration
├── partitions.csv      # Flash partition table (with OTA)
├── sdkconfig.defaults  # ESP-IDF Kconfig settings
├── docs/screenshot.png # Web UI screenshot
└── README.md           # Documentation
```

## Building

```bash
# Build only
pio run

# Build and flash
pio run --target upload

# Monitor serial output
pio device monitor
```

## Key Code Architecture

### Single File Design
All code is in `src/main.c` for simplicity. Main components:

1. **WiFi** - Station mode, connects to configured network
2. **BLE** - NimBLE GATT client, scans and connects to E-Priam
3. **MQTT** - ESP-MQTT client with Home Assistant discovery
4. **HTTP** - ESP-IDF HTTP server for web UI and API
5. **OTA** - esp_ota_ops for firmware updates

### Important Globals
```c
static bool ble_connected = false;
static bool mqtt_connected = false;
static bool is_rocking = false;
static bool auto_renew_enabled = false;
static int battery_percent = -1;  // -1 = unknown
static int drive_mode = -1;       // 1=ECO, 2=TOUR, 3=BOOST, -1=unknown
static int rock_intensity = 100;  // 0-100% (default 100)
static int rock_minutes = 0;
static int auto_renew_duration = 120;  // 2 hours default
static int64_t rock_start_time = 0;
```

### BLE Characteristics (E-Priam Protocol)

Service UUID: `a1fc0101-78d3-40c2-9b6f-3c5f7b2797df`

| Handle | UUID Suffix | Description |
|--------|-------------|-------------|
| ~114 | 0102 | Status (read/notify): battery voltage at byte[3] |
| ~117 | 0103 | Drive mode (write-only): 1=ECO, 2=TOUR, 3=BOOST |
| ~120 | 0104 | Rocking (write/notify): [0x01, min, intensity%] or [0x00] |
| ~123 | 0105 | Battery LEDs (read): 1-3 |

**Note:** Drive mode is write-only - cannot be read back. The firmware tracks mode locally after successful write.

**BLE Notifications:** The firmware subscribes to STATUS and ROCKING notifications for real-time updates (~1/sec).

### Rocking Command Format
```c
// Start rocking
uint8_t cmd[3] = {0x01, minutes, intensity_percent};
ble_gattc_write_flat(conn_handle, 120, cmd, 3, NULL, NULL);

// Stop rocking
uint8_t stop = 0x00;
ble_gattc_write_flat(conn_handle, 120, &stop, 1, NULL, NULL);
```

### Auto-Renew Logic
When `auto_renew_enabled` is true and remaining time < 10 minutes, the firmware automatically sends a new 30-minute rocking command.

## MQTT Topics

Discovery prefix: `homeassistant/`

| Entity | Topic | Type |
|--------|-------|------|
| Battery | sensor/epriam_battery | Sensor |
| Rocking | switch/epriam_rocking | Switch |
| Auto-renew | switch/epriam_autorenew | Switch |
| Mode | select/epriam_mode | Select |
| Duration | select/epriam_duration | Select |
| Intensity | number/epriam_intensity | Number |
| Connected | binary_sensor/epriam_connected | Binary Sensor |
| Remaining | sensor/epriam_remaining | Sensor |
| IP | sensor/epriam_ip | Sensor |

## Web API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main web UI |
| `/config` | GET | Entity name configuration |
| `/ota` | GET | OTA update page |
| `/api/status` | GET | JSON status |
| `/api/log` | GET | BLE log (newest first) |
| `/api/mode/eco` | GET | Set ECO mode |
| `/api/mode/tour` | GET | Set TOUR mode |
| `/api/mode/boost` | GET | Set BOOST mode |
| `/api/rock/start?min=X&intensity=Y` | GET | Start rocking |
| `/api/rock/stop` | GET | Stop rocking |
| `/api/rock/autorenew` | GET | Toggle auto-renew |
| `/api/rescan` | GET | Restart BLE scan |
| `/api/disconnect` | GET | Disconnect BLE |

## Common Development Tasks

### Adding a New MQTT Entity
1. Add discovery message in `mqtt_publish_discovery()`
2. Add state publishing in `mqtt_publish_state()`
3. If writable, add command handler in `mqtt_event_handler()`

### Adding a New Web API Endpoint
1. Create handler function: `static esp_err_t api_xxx(httpd_req_t *req)`
2. Register in `start_webserver()` with `httpd_register_uri_handler()`

### Modifying Web UI
The HTML is embedded as C strings in `root_handler()`. Use minimal HTML/CSS for size.

### Debugging BLE Issues
1. Check web log at `/api/log`
2. Monitor serial output: `pio device monitor`
3. Look for manufacturer ID 0x078D in scan results
4. E-Priam uses random addresses, so address changes between sessions

## Troubleshooting

### BLE Won't Connect
- E-Priam must be powered on
- Move ESP32 closer (< 3m recommended)
- Check that stroller isn't connected to phone app
- Press "Scan" button in web UI
- **Address preference:** Firmware prefers random addresses (type=1) over public (type=0)
- **After using Cybex app:** May need to reset stroller (lower/raise handlebar, power cycle)

### MQTT Not Working
- Verify broker IP/credentials in `main.c`
- Check that Home Assistant MQTT integration is set up
- Entities appear under `homeassistant/` topic prefix

### Build Errors
- Ensure PlatformIO is installed
- Run `pio pkg update` to update dependencies
- Check ESP-IDF version matches 5.5.1

## Configuration

WiFi and MQTT credentials are hardcoded in `main.c`:
```c
#define MQTT_BROKER "mqtt://10.0.0.50"
#define MQTT_USER "epriam"
#define MQTT_PASS "epriam123"
#define WIFI_SSID "YourNetwork"
#define WIFI_PASS "YourPassword"
```

Entity names can be customized via web UI at `/config` and are stored in NVS.

## Memory Considerations

Current usage:
- RAM: ~17% (55KB of 328KB)
- Flash: ~70% (1.4MB of 2MB per OTA partition)

Keep web UI HTML minimal. Avoid large buffers.
