/*
 * Cybex E-Priam BLE Bridge for ESP32-C6 v2.0
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "PRIAM";

// Configurable entity names (stored in NVS)
static char name_device[32] = "Cybex E-Priam";
static char name_battery[32] = "Batteri";
static char name_rocking[32] = "Vugging";
static char name_autorenew[32] = "Auto-forny";
static char name_mode[32] = "Modus";
static char name_intensity[32] = "Intensitet";
static char name_connected[32] = "Tilkoblet";

// MQTT Configuration for Home Assistant
// Change these to your MQTT broker credentials
#define MQTT_BROKER "mqtt://10.0.0.50"
#define MQTT_USER "epriam"              // Your MQTT username
#define MQTT_PASS "epriam123"           // Your MQTT password

#define WIFI_SSID "Bronet"
#define WIFI_PASS "utsikten"

static const ble_uuid128_t PRIAM_SERVICE_UUID = 
    BLE_UUID128_INIT(0xdf, 0x97, 0x27, 0x7b, 0x5f, 0x3c, 0x6f, 0x9b, 
                     0xc2, 0x40, 0xd3, 0x78, 0x01, 0x01, 0xfc, 0xa1);
static const ble_uuid128_t STATUS_CHAR_UUID = 
    BLE_UUID128_INIT(0xdf, 0x97, 0x27, 0x7b, 0x5f, 0x3c, 0x6f, 0x9b, 
                     0xc2, 0x40, 0xd3, 0x78, 0x02, 0x01, 0xfc, 0xa1);
static const ble_uuid128_t DRIVE_MODE_CHAR_UUID = 
    BLE_UUID128_INIT(0xdf, 0x97, 0x27, 0x7b, 0x5f, 0x3c, 0x6f, 0x9b, 
                     0xc2, 0x40, 0xd3, 0x78, 0x03, 0x01, 0xfc, 0xa1);
static const ble_uuid128_t ROCKING_CHAR_UUID = 
    BLE_UUID128_INIT(0xdf, 0x97, 0x27, 0x7b, 0x5f, 0x3c, 0x6f, 0x9b, 
                     0xc2, 0x40, 0xd3, 0x78, 0x04, 0x01, 0xfc, 0xa1);
static const ble_uuid128_t BATTERY_LED_CHAR_UUID = 
    BLE_UUID128_INIT(0xdf, 0x97, 0x27, 0x7b, 0x5f, 0x3c, 0x6f, 0x9b, 
                     0xc2, 0x40, 0xd3, 0x78, 0x05, 0x01, 0xfc, 0xa1);

#define MICROCHIP_MANUFACTURER_ID 0x00CD
#define PRIAM_MANUFACTURER_ID 0x078D

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool ble_connected = false;
static ble_addr_t priam_addr;
static bool priam_found = false;
static uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
static volatile int scan_device_count = 0;
static volatile int scan_cycle_count = 0;
static char last_found_name[32] = "";
static volatile int last_connect_rc = -999;
static volatile int last_connect_status = -999;
static volatile int last_write_rc = -999;
static volatile int last_write_status = -999;

static uint16_t status_val_handle = 0;
static uint16_t drive_mode_val_handle = 0;
static uint16_t rocking_val_handle = 0;
static uint16_t battery_led_val_handle = 0;
static uint16_t service_start_handle = 0;
static uint16_t service_end_handle = 0;
static bool service_found = false;
static bool chars_discovered = false;

static int battery_percent = -1;
static int battery_leds = -1;
static int drive_mode = -1;
static bool is_rocking = false;
static volatile int pending_mode = 0;
static volatile int pending_rock_start = 0;
static volatile int pending_rock_stop = 0;
static volatile int rock_minutes = 5;      // 0 = continuous, 1-30 = timer
static volatile int rock_intensity = 50;   // 0-100%

// Auto-renew rocking mode
static volatile bool auto_renew_enabled = false;
static volatile int64_t rock_start_time = 0;
static volatile int auto_renew_duration = 30;  // 30 min default
static volatile int auto_renew_threshold = 10; // Renew when 10 min left

// MQTT
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Web log buffer (circular)
#define WEB_LOG_SIZE 2048
static char web_log[WEB_LOG_SIZE];
static int web_log_pos = 0;
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;

static void web_log_add(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        portENTER_CRITICAL(&log_mux);
        for (int i = 0; i < len && i < 127; i++) {
            web_log[web_log_pos] = buf[i];
            web_log_pos = (web_log_pos + 1) % WEB_LOG_SIZE;
        }
        web_log[web_log_pos] = '\n';
        web_log_pos = (web_log_pos + 1) % WEB_LOG_SIZE;
        portEXIT_CRITICAL(&log_mux);
    }
}

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static httpd_handle_t server = NULL;

static void ble_app_scan(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void read_all_characteristics(void);
static void process_pending_commands(void);
static void mqtt_publish_state(void);
static void mqtt_publish_discovery(void);
static void auto_renew_task(void *arg);
static void load_entity_names(void);
static void save_entity_names(void);

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool is_priam_device(const struct ble_gap_disc_desc *disc) {
    struct ble_hs_adv_fields fields;
    scan_device_count++;
    
    char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
        disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
        disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
    
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0) {
        // Check name first - more reliable
        if (fields.name && fields.name_len > 0) {
            char name[32];
            int len = fields.name_len < 31 ? fields.name_len : 31;
            memcpy(name, fields.name, len);
            name[len] = 0;
            
            // Log all named devices for debugging
            ESP_LOGI(TAG, "Device: %s (type=%d)", name, disc->addr.type);
            web_log_add("%s %d %s", name, disc->rssi, addr_str);
            
            if (strstr(name, "PRIAM") || strstr(name, "priam") || 
                strstr(name, "RN4871") || strstr(name, "RN487") ||
                strstr(name, "Cybex") || strstr(name, "CYBEX")) {
                strncpy(last_found_name, name, 31);
                last_found_name[31] = 0;
                ESP_LOGI(TAG, "*** E-Priam: %s ***", name);
                web_log_add("*** FOUND: %s ***", name);
                return true;
            }
        } else {
            // Log unnamed devices with manufacturer data
            if (fields.mfg_data_len >= 2) {
                uint16_t mfg_id = fields.mfg_data[0] | (fields.mfg_data[1] << 8);
                web_log_add("(no name) mfg=0x%04X %d %s", mfg_id, disc->rssi, addr_str);
            }
        }
        
        // Check manufacturer ID as backup
        if (fields.mfg_data_len >= 2) {
            uint16_t mfg_id = fields.mfg_data[0] | (fields.mfg_data[1] << 8);
            if (mfg_id == MICROCHIP_MANUFACTURER_ID || mfg_id == PRIAM_MANUFACTURER_ID) {
                ESP_LOGI(TAG, "*** E-Priam by mfg: 0x%04X ***", mfg_id);
                web_log_add("*** FOUND mfg: 0x%04X ***", mfg_id);
                return true;
            }
        }
    }
    return false;
}

static int on_status_read(uint16_t ch, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg) {
    if (e->status == 0 && a) {
        uint8_t d[20];
        uint16_t l = OS_MBUF_PKTLEN(a->om);
        if (l > 20) l = 20;
        os_mbuf_copydata(a->om, 0, l, d);
        if (l >= 4) {
            battery_percent = (d[3] * 100) / 255;
            ESP_LOGI(TAG, "Batt: %d%%", battery_percent);
        }
    }
    return 0;
}

static int on_drive_read(uint16_t ch, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg) {
    ESP_LOGI(TAG, "on_drive_read: status=%d", e->status);
    if (e->status == 0 && a) {
        uint8_t d[8];
        uint16_t l = OS_MBUF_PKTLEN(a->om);
        if (l > 8) l = 8;
        os_mbuf_copydata(a->om, 0, l, d);
        ESP_LOGI(TAG, "Mode raw: len=%d, [%02X %02X %02X %02X]", l, d[0], d[1], d[2], d[3]);
        if (l >= 1) {
            drive_mode = d[0];
            ESP_LOGI(TAG, "Mode: %d", drive_mode);
        }
    }
    return 0;
}

static int on_led_read(uint16_t ch, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg) {
    if (e->status == 0 && a) {
        uint8_t d[8];
        uint16_t l = OS_MBUF_PKTLEN(a->om);
        if (l > 8) l = 8;
        os_mbuf_copydata(a->om, 0, l, d);
        if (l >= 1) {
            battery_leds = d[0];
            ESP_LOGI(TAG, "LEDs: %d", battery_leds);
        }
    }
    process_pending_commands();
    return 0;
}

static int on_write(uint16_t ch, const struct ble_gatt_error *e, struct ble_gatt_attr *a, void *arg) {
    last_write_status = e->status;
    ESP_LOGI(TAG, "Write callback: status=%d", e->status);
    return 0;
}

static void read_all_characteristics(void) {
    if (!ble_connected || !chars_discovered) return;
    if (status_val_handle) ble_gattc_read(conn_handle, status_val_handle, on_status_read, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (drive_mode_val_handle) ble_gattc_read(conn_handle, drive_mode_val_handle, on_drive_read, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (battery_led_val_handle) ble_gattc_read(conn_handle, battery_led_val_handle, on_led_read, NULL);
}

static void process_pending_commands(void) {
    ESP_LOGI(TAG, "process_pending: mode=%d, rock_start=%d, rock_stop=%d, connected=%d, chars=%d",
        pending_mode, pending_rock_start, pending_rock_stop, ble_connected, chars_discovered);
    if (!ble_connected || !chars_discovered) return;
    if (pending_mode > 0 && drive_mode_val_handle) {
        uint8_t m = pending_mode;
        pending_mode = 0;
        ESP_LOGI(TAG, "Writing mode %d to handle %d (conn=%d)", m, drive_mode_val_handle, conn_handle);
        last_write_rc = ble_gattc_write_flat(conn_handle, drive_mode_val_handle, &m, 1, on_write, NULL);
        ESP_LOGI(TAG, "Write rc=%d", last_write_rc);
        vTaskDelay(pdMS_TO_TICKS(500));
        ble_gattc_read(conn_handle, drive_mode_val_handle, on_drive_read, NULL);
    }
    if (pending_rock_start && rocking_val_handle) {
        pending_rock_start = 0;
        // Format: [0x01, minutes (0=continuous), intensity%]
        uint8_t cmd[3] = {0x01, (uint8_t)rock_minutes, (uint8_t)rock_intensity};
        ESP_LOGI(TAG, "Rock start: %d min, %d%%", rock_minutes, rock_intensity);
        ble_gattc_write_flat(conn_handle, rocking_val_handle, cmd, 3, on_write, NULL);
        is_rocking = true;
        rock_start_time = esp_timer_get_time() / 1000000;  // Set start time in seconds
    }
    if (pending_rock_stop && rocking_val_handle) {
        pending_rock_stop = 0;
        uint8_t cmd = 0x00;
        ble_gattc_write_flat(conn_handle, rocking_val_handle, &cmd, 1, on_write, NULL);
        is_rocking = false;
    }
}

static int on_chr(uint16_t ch, const struct ble_gatt_error *e, const struct ble_gatt_chr *c, void *arg) {
    if (e->status == 0 && c) {
        if (ble_uuid_cmp(&c->uuid.u, &STATUS_CHAR_UUID.u) == 0) {
            status_val_handle = c->val_handle;
            ESP_LOGI(TAG, "STATUS: %d", c->val_handle);
        } else if (ble_uuid_cmp(&c->uuid.u, &DRIVE_MODE_CHAR_UUID.u) == 0) {
            drive_mode_val_handle = c->val_handle;
            ESP_LOGI(TAG, "MODE: %d", c->val_handle);
        } else if (ble_uuid_cmp(&c->uuid.u, &ROCKING_CHAR_UUID.u) == 0) {
            rocking_val_handle = c->val_handle;
            ESP_LOGI(TAG, "ROCK: %d", c->val_handle);
        } else if (ble_uuid_cmp(&c->uuid.u, &BATTERY_LED_CHAR_UUID.u) == 0) {
            battery_led_val_handle = c->val_handle;
            ESP_LOGI(TAG, "LED: %d", c->val_handle);
        }
    } else if (e->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Chars done: %d,%d,%d,%d", status_val_handle, drive_mode_val_handle, rocking_val_handle, battery_led_val_handle);
        if (status_val_handle || drive_mode_val_handle) {
            chars_discovered = true;
            vTaskDelay(pdMS_TO_TICKS(200));
            read_all_characteristics();
        }
    }
    return 0;
}

static int on_svc(uint16_t ch, const struct ble_gatt_error *e, const struct ble_gatt_svc *s, void *arg) {
    if (e->status == 0 && s) {
        if (ble_uuid_cmp(&s->uuid.u, &PRIAM_SERVICE_UUID.u) == 0) {
            ESP_LOGI(TAG, "*** Found service! ***");
            service_found = true;
            service_start_handle = s->start_handle;
            service_end_handle = s->end_handle;
        }
    } else if (e->status == BLE_HS_EDONE) {
        if (service_found) {
            ble_gattc_disc_all_chrs(conn_handle, service_start_handle, service_end_handle, on_chr, NULL);
        } else {
            ble_gattc_disc_all_chrs(conn_handle, 1, 0xFFFF, on_chr, NULL);
        }
    }
    return 0;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (!priam_found && is_priam_device(&event->disc)) {
                char addr_str[18];
                snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                    event->disc.addr.val[5], event->disc.addr.val[4],
                    event->disc.addr.val[3], event->disc.addr.val[2],
                    event->disc.addr.val[1], event->disc.addr.val[0]);
                ESP_LOGI(TAG, "E-Priam addr: %s (type=%d)", addr_str, event->disc.addr.type);
                web_log_add("Connecting to %s (type=%d)...", addr_str, event->disc.addr.type);
                memcpy(&priam_addr, &event->disc.addr, sizeof(ble_addr_t));
                priam_found = true;
                
                // Cancel scan and connect immediately
                ble_gap_disc_cancel();
                
                // Connect directly without waiting for DISC_COMPLETE
                ESP_LOGI(TAG, "Connecting immediately to %s...", addr_str);
                service_found = false;
                chars_discovered = false;
                status_val_handle = 0;
                drive_mode_val_handle = 0;
                rocking_val_handle = 0;
                battery_led_val_handle = 0;
                
                last_connect_rc = ble_gap_connect(own_addr_type, &priam_addr, 30000, NULL, ble_gap_event, NULL);
                ESP_LOGI(TAG, "ble_gap_connect rc=%d", last_connect_rc);
                if (last_connect_rc != 0) {
                    ESP_LOGE(TAG, "Connect failed immediately: %d", last_connect_rc);
                    web_log_add("Connect failed: rc=%d", last_connect_rc);
                    priam_found = false;
                }
            }
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "DISC_COMPLETE: reason=%d, priam_found=%d", 
                event->disc_complete.reason, priam_found);
            if (!priam_found && !ble_connected) {
                ESP_LOGI(TAG, "E-Priam not found, rescanning...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                ble_app_scan();
            }
            break;
        case BLE_GAP_EVENT_CONNECT:
            last_connect_status = event->connect.status;
            ESP_LOGI(TAG, "CONNECT event: status=%d", event->connect.status);
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                ble_connected = true;
                ESP_LOGI(TAG, "*** CONNECTED (handle=%d) ***", conn_handle);
                web_log_add("*** CONNECTED ***");
                ble_gattc_disc_all_svcs(conn_handle, on_svc, NULL);
            } else {
                ESP_LOGE(TAG, "Connect failed: status=%d", event->connect.status);
                web_log_add("Connection failed: %d", event->connect.status);
                priam_found = false;
                vTaskDelay(pdMS_TO_TICKS(3000));
                ble_app_scan();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "DISCONNECT: reason=0x%04X", event->disconnect.reason);
            web_log_add("Disconnected: 0x%04X", event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_connected = false;
            priam_found = false;
            chars_discovered = false;
            battery_percent = -1;
            battery_leds = -1;
            drive_mode = -1;
            is_rocking = false;
            status_val_handle = 0;
            drive_mode_val_handle = 0;
            rocking_val_handle = 0;
            battery_led_val_handle = 0;
            vTaskDelay(pdMS_TO_TICKS(2000));
            ble_app_scan();
            break;
        default:
            break;
    }
    return 0;
}

static void ble_app_scan(void) {
    struct ble_gap_disc_params dp = {
        .itvl = 0,
        .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0  // Allow duplicates to see repeated advertisements
    };
    scan_cycle_count++;
    scan_device_count = 0;
    ESP_LOGI(TAG, "Scan cycle #%d starting (30s)...", scan_cycle_count);
    web_log_add("Scan #%d started...", scan_cycle_count);
    priam_found = false;
    int rc = ble_gap_disc(own_addr_type, 30000, &dp, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan start failed: %d", rc);
        web_log_add("Scan failed: %d", rc);
        vTaskDelay(pdMS_TO_TICKS(5000));
        ble_app_scan();
    }
}

static void ble_host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) {
    ESP_LOGI(TAG, "BLE sync");
    
    // Configure random address for connecting to devices with random addresses
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Address infer failed: %d", rc);
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    } else {
        ESP_LOGI(TAG, "Own addr type: %d", own_addr_type);
    }
    
    // Print our address
    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "Our addr: %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], own_addr_type);
    }
    
    ble_app_scan();
}

static void ble_on_reset(int r) {
    ESP_LOGE(TAG, "BLE reset: %d", r);
}

static void ble_init(void) {
    if (nimble_port_init() != ESP_OK) return;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_svc_gap_device_name_set("EPriam-Bridge");
    nimble_port_freertos_init(ble_host_task);
}

// Auto-renew task - runs every minute to check if rocking needs renewal
static void auto_renew_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));  // Check every minute
        
        if (auto_renew_enabled && is_rocking && ble_connected && chars_discovered) {
            int64_t now = esp_timer_get_time() / 1000000;  // seconds
            int64_t elapsed = now - rock_start_time;
            int remaining = (auto_renew_duration * 60) - (int)elapsed;
            
            ESP_LOGI(TAG, "Auto-renew check: elapsed=%llds, remaining=%ds", elapsed, remaining);
            
            // Renew when threshold minutes remaining
            if (remaining <= (auto_renew_threshold * 60) && remaining > 0) {
                ESP_LOGI(TAG, "*** Auto-renewing rocking for %d min ***", auto_renew_duration);
                rock_minutes = auto_renew_duration;
                rock_start_time = now;  // Reset timer
                pending_rock_start = 1;
                process_pending_commands();
                mqtt_publish_state();
            }
        }
    }
}

// MQTT event handler
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t event = data;
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected!");
            mqtt_connected = true;
            // Publish discovery configs first
            mqtt_publish_discovery();
            vTaskDelay(pdMS_TO_TICKS(500));
            // Subscribe to command topics
            esp_mqtt_client_subscribe(mqtt_client, "homeassistant/switch/epriam_rocking/set", 0);
            esp_mqtt_client_subscribe(mqtt_client, "homeassistant/select/epriam_mode/set", 0);
            esp_mqtt_client_subscribe(mqtt_client, "homeassistant/switch/epriam_autorenew/set", 0);
            esp_mqtt_client_subscribe(mqtt_client, "homeassistant/number/epriam_intensity/set", 0);
            // Publish current state
            mqtt_publish_state();
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGI(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            {
                char topic[128], payload[64];
                int tlen = event->topic_len < 127 ? event->topic_len : 127;
                int plen = event->data_len < 63 ? event->data_len : 63;
                memcpy(topic, event->topic, tlen); topic[tlen] = 0;
                memcpy(payload, event->data, plen); payload[plen] = 0;
                ESP_LOGI(TAG, "MQTT: %s = %s", topic, payload);
                
                if (strstr(topic, "rocking/set")) {
                    if (strcmp(payload, "ON") == 0) {
                        if (auto_renew_enabled) {
                            rock_minutes = auto_renew_duration;
                            rock_start_time = esp_timer_get_time() / 1000000;
                        }
                        pending_rock_start = 1;
                    } else {
                        pending_rock_stop = 1;
                        auto_renew_enabled = false;
                    }
                    if (ble_connected && chars_discovered) process_pending_commands();
                    mqtt_publish_state();
                }
                else if (strstr(topic, "mode/set")) {
                    if (strcmp(payload, "ECO") == 0) pending_mode = 1;
                    else if (strcmp(payload, "TOUR") == 0) pending_mode = 2;
                    else if (strcmp(payload, "BOOST") == 0) pending_mode = 3;
                    if (ble_connected && chars_discovered) process_pending_commands();
                    mqtt_publish_state();
                }
                else if (strstr(topic, "autorenew/set")) {
                    auto_renew_enabled = (strcmp(payload, "ON") == 0);
                    if (auto_renew_enabled && is_rocking) {
                        rock_start_time = esp_timer_get_time() / 1000000;
                    }
                    ESP_LOGI(TAG, "Auto-renew: %s", auto_renew_enabled ? "ON" : "OFF");
                    mqtt_publish_state();
                }
                else if (strstr(topic, "intensity/set")) {
                    int i = atoi(payload);
                    if (i >= 0 && i <= 100) rock_intensity = i;
                    mqtt_publish_state();
                }
            }
            break;
        default:
            break;
    }
}

static void mqtt_publish_state(void) {
    if (!mqtt_connected) return;
    
    char buf[64];
    
    // Battery sensor
    snprintf(buf, 64, "%d", battery_percent);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_battery/state", buf, 0, 0, true);
    
    // Rocking switch state
    esp_mqtt_client_publish(mqtt_client, "homeassistant/switch/epriam_rocking/state", 
        is_rocking ? "ON" : "OFF", 0, 0, true);
    
    // Auto-renew switch state
    esp_mqtt_client_publish(mqtt_client, "homeassistant/switch/epriam_autorenew/state",
        auto_renew_enabled ? "ON" : "OFF", 0, 0, true);
    
    // Mode select state
    const char* mode = drive_mode == 1 ? "ECO" : drive_mode == 2 ? "TOUR" : drive_mode == 3 ? "BOOST" : "UNKNOWN";
    esp_mqtt_client_publish(mqtt_client, "homeassistant/select/epriam_mode/state", mode, 0, 0, true);
    
    // Intensity number
    snprintf(buf, 64, "%d", rock_intensity);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/number/epriam_intensity/state", buf, 0, 0, true);
    
    // Connection status
    esp_mqtt_client_publish(mqtt_client, "homeassistant/binary_sensor/epriam_connected/state",
        ble_connected ? "ON" : "OFF", 0, 0, true);
    
    // Remaining time sensor
    int remaining = 0;
    if (is_rocking && rock_minutes > 0 && rock_start_time > 0) {
        int64_t now = esp_timer_get_time() / 1000000;
        int elapsed = (int)(now - rock_start_time);
        remaining = (rock_minutes * 60) - elapsed;
        if (remaining < 0) remaining = 0;
    }
    snprintf(buf, 64, "%d", remaining);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_remaining/state", buf, 0, 0, true);
    
    // IP address sensor
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, 64, IPSTR, IP2STR(&ip_info.ip));
        esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_ip/state", buf, 0, 0, true);
    }
}

static void mqtt_publish_discovery(void) {
    if (!mqtt_connected) return;
    
    static char buf[400];
    
    // Battery sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_battery\","
        "\"state_topic\":\"homeassistant/sensor/epriam_battery/state\","
        "\"device_class\":\"battery\",\"unit_of_measurement\":\"%%\","
        "\"device\":{\"identifiers\":[\"epriam\"],\"name\":\"%s\",\"manufacturer\":\"Cybex\"}}",
        name_battery, name_device);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_battery/config", buf, 0, 0, true);
    
    // Rocking switch
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_rocking\","
        "\"state_topic\":\"homeassistant/switch/epriam_rocking/state\","
        "\"command_topic\":\"homeassistant/switch/epriam_rocking/set\","
        "\"icon\":\"mdi:baby-carriage\",\"device\":{\"identifiers\":[\"epriam\"]}}",
        name_rocking);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/switch/epriam_rocking/config", buf, 0, 0, true);
    
    // Auto-renew switch
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_autorenew\","
        "\"state_topic\":\"homeassistant/switch/epriam_autorenew/state\","
        "\"command_topic\":\"homeassistant/switch/epriam_autorenew/set\","
        "\"icon\":\"mdi:autorenew\",\"device\":{\"identifiers\":[\"epriam\"]}}",
        name_autorenew);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/switch/epriam_autorenew/config", buf, 0, 0, true);
    
    // Mode select
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_mode\","
        "\"state_topic\":\"homeassistant/select/epriam_mode/state\","
        "\"command_topic\":\"homeassistant/select/epriam_mode/set\","
        "\"options\":[\"ECO\",\"TOUR\",\"BOOST\"],\"icon\":\"mdi:speedometer\","
        "\"device\":{\"identifiers\":[\"epriam\"]}}",
        name_mode);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/select/epriam_mode/config", buf, 0, 0, true);
    
    // Intensity slider
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_intensity\","
        "\"state_topic\":\"homeassistant/number/epriam_intensity/state\","
        "\"command_topic\":\"homeassistant/number/epriam_intensity/set\","
        "\"min\":0,\"max\":100,\"step\":10,\"icon\":\"mdi:vibrate\","
        "\"device\":{\"identifiers\":[\"epriam\"]}}",
        name_intensity);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/number/epriam_intensity/config", buf, 0, 0, true);
    
    // BLE Connected binary sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"unique_id\":\"epriam_connected\","
        "\"state_topic\":\"homeassistant/binary_sensor/epriam_connected/state\","
        "\"device_class\":\"connectivity\",\"device\":{\"identifiers\":[\"epriam\"]}}",
        name_connected);
    esp_mqtt_client_publish(mqtt_client, "homeassistant/binary_sensor/epriam_connected/config", buf, 0, 0, true);
    
    // Remaining time sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"Gjenstående tid\",\"unique_id\":\"epriam_remaining\","
        "\"state_topic\":\"homeassistant/sensor/epriam_remaining/state\","
        "\"unit_of_measurement\":\"s\",\"icon\":\"mdi:timer-outline\","
        "\"device\":{\"identifiers\":[\"epriam\"]}}");
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_remaining/config", buf, 0, 0, true);
    
    // IP Address sensor
    snprintf(buf, sizeof(buf),
        "{\"name\":\"IP-adresse\",\"unique_id\":\"epriam_ip\","
        "\"state_topic\":\"homeassistant/sensor/epriam_ip/state\","
        "\"icon\":\"mdi:ip-network\",\"device\":{\"identifiers\":[\"epriam\"]}}");
    esp_mqtt_client_publish(mqtt_client, "homeassistant/sensor/epriam_ip/config", buf, 0, 0, true);
    
    ESP_LOGI(TAG, "MQTT discovery published with custom names");
}

// Load entity names from NVS
static void load_entity_names(void) {
    nvs_handle_t nvs;
    if (nvs_open("epriam", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(name_device); nvs_get_str(nvs, "n_device", name_device, &len);
        len = sizeof(name_battery); nvs_get_str(nvs, "n_battery", name_battery, &len);
        len = sizeof(name_rocking); nvs_get_str(nvs, "n_rocking", name_rocking, &len);
        len = sizeof(name_autorenew); nvs_get_str(nvs, "n_autorenew", name_autorenew, &len);
        len = sizeof(name_mode); nvs_get_str(nvs, "n_mode", name_mode, &len);
        len = sizeof(name_intensity); nvs_get_str(nvs, "n_intensity", name_intensity, &len);
        len = sizeof(name_connected); nvs_get_str(nvs, "n_connected", name_connected, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Loaded entity names from NVS");
    }
}

// Save entity names to NVS
static void save_entity_names(void) {
    nvs_handle_t nvs;
    if (nvs_open("epriam", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "n_device", name_device);
        nvs_set_str(nvs, "n_battery", name_battery);
        nvs_set_str(nvs, "n_rocking", name_rocking);
        nvs_set_str(nvs, "n_autorenew", name_autorenew);
        nvs_set_str(nvs, "n_mode", name_mode);
        nvs_set_str(nvs, "n_intensity", name_intensity);
        nvs_set_str(nvs, "n_connected", name_connected);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved entity names to NVS");
    }
}

static void mqtt_init(void) {
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    if (mqtt_client) {
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
        ESP_LOGI(TAG, "MQTT client started (will connect in background)");
    } else {
        ESP_LOGE(TAG, "Failed to init MQTT client");
    }
}

static esp_err_t root_handler(httpd_req_t *req) {
    char batt[16];
    if (battery_percent >= 0) snprintf(batt, 16, "%d%%", battery_percent);
    else strcpy(batt, "?");
    
    const char* mode = drive_mode == 1 ? "ECO" : drive_mode == 2 ? "TOUR" : drive_mode == 3 ? "BOOST" : "?";
    const char* auto_status = auto_renew_enabled ? "on" : "off";
    
    static char r[3400];
    snprintf(r, sizeof(r),
        "<!DOCTYPE html><html><head><meta charset=UTF-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>E-Priam</title><style>*{box-sizing:border-box}body{font-family:Arial;max-width:400px;margin:auto;padding:10px;background:#1a1a2e;color:#eee}"
        ".c{background:#16213e;padding:15px;border-radius:8px;margin:8px 0}.b{padding:12px 20px;margin:3px;border:none;border-radius:6px;cursor:pointer}"
        ".st{display:flex;flex-wrap:wrap;gap:8px}.si{flex:1;min-width:60px;text-align:center;padding:8px;background:#0a0a14;border-radius:6px}"
        ".si b{font-size:18px}.si small{color:#888;font-size:10px;display:block}"
        ".on{color:#4CAF50}.off{color:#f44336}#cd{font-size:24px;color:#FF9800;font-weight:bold}"
        "a{color:#4CAF50}#log{background:#0a0a14;padding:8px;font-family:monospace;font-size:11px;max-height:150px;overflow-y:auto;white-space:pre-wrap}</style></head><body>"
        "<h2 style=margin-bottom:5px>🍼 E-Priam</h2>"
        "<div class=c><div class=st>"
        "<div class=si><b class=%s>●</b><small>BLE</small></div>"
        "<div class=si><b class=%s>●</b><small>MQTT</small></div>"
        "<div class=si><b>🔋%s</b><small>Battery</small></div>"
        "<div class=si><b>%s</b><small>Mode</small></div>"
        "<div class=si><b class=%s>∞</b><small>Auto</small></div>"
        "</div></div>"
        "<div class=c id=rockbox style='text-align:center;display:none'><div id=cd>--:--</div><small>Remaining</small></div>"
        "<div class=c><b class=b style=background:#4CAF50 onclick=\"fetch('/api/mode/eco').then(st)\">ECO</b>"
        "<b class=b style=background:#2196F3 onclick=\"fetch('/api/mode/tour').then(st)\">TOUR</b>"
        "<b class=b style=background:#f44336 onclick=\"fetch('/api/mode/boost').then(st)\">BOOST</b></div>"
        "<div class=c><b class=b style=background:#9C27B0 onclick=\"fetch('/api/rock/start').then(st)\">5m</b>"
        "<b class=b style=background:#9C27B0 onclick=\"fetch('/api/rock/start?min=15').then(st)\">15m</b>"
        "<b class=b style=background:#9C27B0 onclick=\"fetch('/api/rock/start?min=30').then(st)\">30m</b></div>"
        "<div class=c><b id=abtn class=b style=background:%s onclick=\"fetch('/api/rock/autorenew').then(st)\">∞ Auto</b>"
        "<b class=b style=background:#666 onclick=\"fetch('/api/rock/stop').then(st)\">Stop</b></div>"
        "<div class=c>%s<a href=/config>⚙ Config</a> <a href=/ota style=float:right>🔄 OTA</a></div>"
        "<div class=c><b>BLE Log:</b><div id=log>Loading...</div></div>"
        "<script>var ar=%s;function st(){fetch('/api/status').then(r=>r.json()).then(d=>{rs=d.remaining_sec;rk=d.rocking;ar=d.auto_renew;"
        "document.getElementById('rockbox').style.display=rk?'block':'none';document.getElementById('abtn').style.background=ar?'#4CAF50':'#FF9800';upcd()})}"
        "var rs=0,rk=false;function upcd(){if(!rk||rs<=0){document.getElementById('cd').textContent='--:--';return;}"
        "var m=Math.floor(rs/60),s=rs%%60;document.getElementById('cd').textContent=m+':'+(s<10?'0':'')+s}"
        "function upd(){fetch('/api/log').then(r=>r.text()).then(t=>{document.getElementById('log').textContent=t})}"
        "st();upd();setInterval(()=>{if(rk&&rs>0){rs--;upcd()}},1000);setInterval(st,10000);setInterval(upd,5000)</script></body></html>",
        ble_connected ? "on" : "off", mqtt_connected ? "on" : "off", batt, mode, auto_status,
        auto_renew_enabled ? "#4CAF50" : "#FF9800",
        ble_connected ? "" : "<b class=b style=background:#FF5722 onclick=\"fetch('/api/rescan').then(()=>setTimeout(()=>location.reload(),3000))\">🔍 Scan</b>",
        auto_renew_enabled ? "true" : "false");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, r, strlen(r));
    return ESP_OK;
}

static esp_err_t api_status(httpd_req_t *req) {
    // Calculate remaining seconds
    int remaining_sec = 0;
    if (is_rocking && rock_start_time > 0) {
        int64_t now = esp_timer_get_time() / 1000000; // to seconds
        int64_t elapsed = now - rock_start_time;  // rock_start_time is already in seconds
        int duration_sec = auto_renew_enabled ? (auto_renew_duration * 60) : (rock_minutes * 60);
        remaining_sec = duration_sec - (int)elapsed;
        if (remaining_sec < 0) remaining_sec = 0;
    }
    char r[450];
    snprintf(r, sizeof(r), "{\"connected\":%s,\"mqtt\":%s,\"battery\":%d,\"battery_leds\":%d,\"drive_mode\":%d,\"rocking\":%s,\"auto_renew\":%s,\"intensity\":%d,\"remaining_sec\":%d,\"rock_minutes\":%d}",
        ble_connected ? "true" : "false", mqtt_connected ? "true" : "false",
        battery_percent, battery_leds, drive_mode, 
        is_rocking ? "true" : "false", auto_renew_enabled ? "true" : "false", rock_intensity, remaining_sec, rock_minutes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, r, strlen(r));
    return ESP_OK;
}

static esp_err_t api_rescan(httpd_req_t *req) {
    if (!ble_connected) {
        ESP_LOGI(TAG, "Manual rescan triggered");
        web_log_add("Manual scan started");
        ble_gap_disc_cancel();  // Cancel any ongoing scan
        vTaskDelay(pdMS_TO_TICKS(100));
        ble_app_scan();  // Start new scan
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Scanning...\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Already connected\"}");
    }
    return ESP_OK;
}

// Log endpoint - returns circular log buffer (newest first)
static esp_err_t api_log(httpd_req_t *req) {
    static char resp[WEB_LOG_SIZE + 64];
    static char temp[WEB_LOG_SIZE + 64];
    portENTER_CRITICAL(&log_mux);
    // Copy log in order (oldest to newest)
    int idx = 0;
    for (int i = 0; i < WEB_LOG_SIZE; i++) {
        int pos = (web_log_pos + i) % WEB_LOG_SIZE;
        if (web_log[pos] != 0) {
            temp[idx++] = web_log[pos];
        }
    }
    temp[idx] = 0;
    portEXIT_CRITICAL(&log_mux);
    
    // Reverse lines (newest first)
    int resp_idx = 0;
    int end = idx;
    while (end > 0) {
        // Find start of last line
        int start = end - 1;
        while (start > 0 && temp[start - 1] != '\n') start--;
        // Copy this line
        for (int j = start; j < end; j++) {
            resp[resp_idx++] = temp[j];
        }
        if (start > 0 && temp[start - 1] == '\n') {
            end = start - 1;
        } else {
            end = start;
        }
        if (end > 0) resp[resp_idx++] = '\n';
    }
    resp[resp_idx] = 0;
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t api_eco(httpd_req_t *req) {
    pending_mode = 1;
    if (ble_connected && chars_discovered) process_pending_commands();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_tour(httpd_req_t *req) {
    pending_mode = 2;
    if (ble_connected && chars_discovered) process_pending_commands();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_boost(httpd_req_t *req) {
    pending_mode = 3;
    if (ble_connected && chars_discovered) process_pending_commands();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_rock_start(httpd_req_t *req) {
    // Parse query string for minutes and intensity
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "min", param, sizeof(param)) == ESP_OK) {
            int m = atoi(param);
            if (m >= 0 && m <= 30) rock_minutes = m;
        }
        if (httpd_query_key_value(buf, "intensity", param, sizeof(param)) == ESP_OK) {
            int i = atoi(param);
            if (i >= 0 && i <= 100) rock_intensity = i;
        }
    }
    pending_rock_start = 1;
    if (ble_connected && chars_discovered) process_pending_commands();
    char resp[80];
    snprintf(resp, 80, "{\"ok\":true,\"minutes\":%d,\"intensity\":%d}", rock_minutes, rock_intensity);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Continuous rocking (no timer)
static esp_err_t api_rock_continuous(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "intensity", param, sizeof(param)) == ESP_OK) {
            int i = atoi(param);
            if (i >= 0 && i <= 100) rock_intensity = i;
        }
    }
    rock_minutes = 0;  // 0 = continuous
    pending_rock_start = 1;
    if (ble_connected && chars_discovered) process_pending_commands();
    mqtt_publish_state();
    char resp[80];
    snprintf(resp, 80, "{\"ok\":true,\"continuous\":true,\"intensity\":%d}", rock_intensity);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// Auto-renew rocking: 30 min, renews at 10 min remaining
static esp_err_t api_rock_autorenew(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "intensity", param, sizeof(param)) == ESP_OK) {
            int i = atoi(param);
            if (i >= 0 && i <= 100) rock_intensity = i;
        }
    }
    rock_minutes = 30;  // Start with 30 min
    auto_renew_enabled = true;
    rock_start_time = esp_timer_get_time() / 1000000;  // Current time in seconds
    pending_rock_start = 1;
    if (ble_connected && chars_discovered) process_pending_commands();
    mqtt_publish_state();
    char resp[100];
    snprintf(resp, 100, "{\"ok\":true,\"autorenew\":true,\"duration\":30,\"threshold\":10,\"intensity\":%d}", rock_intensity);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_rock_stop(httpd_req_t *req) {
    pending_rock_stop = 1;
    auto_renew_enabled = false;  // Stop auto-renew when stopping
    if (ble_connected && chars_discovered) process_pending_commands();
    mqtt_publish_state();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t api_disconnect(httpd_req_t *req) {
    if (ble_connected && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Config page - entity name settings
static esp_err_t config_handler(httpd_req_t *req) {
    static char html[2048];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset=UTF-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Config</title><style>*{box-sizing:border-box}body{font-family:Arial;max-width:400px;margin:auto;padding:10px;background:#1a1a2e;color:#eee}"
        "input{width:100%%;padding:8px;margin:4px 0;border-radius:4px;border:1px solid #444;background:#16213e;color:#eee}"
        ".b{padding:12px 20px;margin:10px 0;border:none;border-radius:6px;cursor:pointer;background:#4CAF50;color:white;width:100%%}"
        "</style></head><body><h2>Innstillinger</h2>"
        "<form action=/api/config method=POST>"
        "<label>Enhetsnavn:</label><input name=device value=\"%s\">"
        "<label>Batteri:</label><input name=battery value=\"%s\">"
        "<label>Vugging:</label><input name=rocking value=\"%s\">"
        "<label>Auto-forny:</label><input name=autorenew value=\"%s\">"
        "<label>Modus:</label><input name=mode value=\"%s\">"
        "<label>Intensitet:</label><input name=intensity value=\"%s\">"
        "<label>Tilkoblet:</label><input name=connected value=\"%s\">"
        "<button class=b type=submit>Lagre</button></form>"
        "<a href=/><button class=b style=background:#666>Tilbake</button></a></body></html>",
        name_device, name_battery, name_rocking, name_autorenew, name_mode, name_intensity, name_connected);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

// URL decode helper
static void url_decode(char *dst, const char *src, size_t max) {
    char a, b;
    size_t i = 0;
    while (*src && i < max - 1) {
        if (*src == '%' && (a = src[1]) && (b = src[2]) && isxdigit(a) && isxdigit(b)) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= 'A' - 10; else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= 'A' - 10; else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

// POST handler for config
static esp_err_t api_config_post(httpd_req_t *req) {
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        char val[32], decoded[32];
        
        if (httpd_query_key_value(buf, "device", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_device, decoded, sizeof(name_device) - 1);
        }
        if (httpd_query_key_value(buf, "battery", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_battery, decoded, sizeof(name_battery) - 1);
        }
        if (httpd_query_key_value(buf, "rocking", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_rocking, decoded, sizeof(name_rocking) - 1);
        }
        if (httpd_query_key_value(buf, "autorenew", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_autorenew, decoded, sizeof(name_autorenew) - 1);
        }
        if (httpd_query_key_value(buf, "mode", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_mode, decoded, sizeof(name_mode) - 1);
        }
        if (httpd_query_key_value(buf, "intensity", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_intensity, decoded, sizeof(name_intensity) - 1);
        }
        if (httpd_query_key_value(buf, "connected", val, sizeof(val)) == ESP_OK) {
            url_decode(decoded, val, sizeof(decoded));
            strncpy(name_connected, decoded, sizeof(name_connected) - 1);
        }
        
        save_entity_names();
        mqtt_publish_discovery();  // Re-publish with new names
    }
    
    // Redirect back to config page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/config");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_debug(httpd_req_t *req) {
    char addr_str[18] = "none";
    if (priam_found || ble_connected) {
        snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
            priam_addr.val[5], priam_addr.val[4], priam_addr.val[3],
            priam_addr.val[2], priam_addr.val[1], priam_addr.val[0]);
    }
    char r[900];
    snprintf(r, 900, 
        "{\"scan_cycles\":%d,\"priam_found\":%s,"
        "\"priam_addr\":\"%s\",\"priam_addr_type\":%d,"
        "\"ble_connected\":%s,\"conn_handle\":%d,"
        "\"service_found\":%s,\"chars_discovered\":%s,"
        "\"h_status\":%d,\"h_mode\":%d,\"h_rock\":%d,\"h_led\":%d,"
        "\"last_write_rc\":%d,\"last_write_status\":%d,\"pending_mode\":%d}",
        scan_cycle_count, priam_found ? "true" : "false",
        addr_str, priam_addr.type,
        ble_connected ? "true" : "false", conn_handle,
        service_found ? "true" : "false", chars_discovered ? "true" : "false",
        status_val_handle, drive_mode_val_handle, rocking_val_handle, battery_led_val_handle,
        last_write_rc, last_write_status, pending_mode);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, r, strlen(r));
    return ESP_OK;
}

// OTA update handlers
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;

static esp_err_t ota_page_handler(httpd_req_t *req) {
    const char *html = 
        "<!DOCTYPE html><html><head><meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>OTA Update</title><style>body{font-family:Arial;max-width:400px;margin:auto;padding:20px;background:#1a1a2e;color:#eee}"
        ".c{background:#16213e;padding:20px;border-radius:8px;margin:10px 0}input[type=file]{margin:10px 0}"
        ".b{padding:12px 24px;border:none;border-radius:6px;cursor:pointer;background:#4CAF50;color:white;font-size:16px}"
        "#p{width:100%;height:20px;background:#333;border-radius:10px;margin:10px 0}#pb{height:100%;background:#4CAF50;border-radius:10px;width:0%;transition:width 0.3s}"
        "</style></head><body><h2>🔄 OTA Update</h2>"
        "<div class=c><form id=f><input type=file name=fw id=fw accept='.bin'><br>"
        "<div id=p style='display:none'><div id=pb></div></div>"
        "<div id=st></div><br><button type=submit class=b>Upload Firmware</button></form></div>"
        "<div class=c><a href='/'>← Tilbake</a></div>"
        "<script>"
        "document.getElementById('f').onsubmit=async e=>{"
        "e.preventDefault();let f=document.getElementById('fw').files[0];if(!f)return alert('Velg fil');"
        "document.getElementById('p').style.display='block';document.getElementById('st').textContent='Laster opp...';"
        "let xhr=new XMLHttpRequest();xhr.open('POST','/api/ota',true);"
        "xhr.upload.onprogress=e=>{if(e.lengthComputable){let p=Math.round(e.loaded/e.total*100);document.getElementById('pb').style.width=p+'%';document.getElementById('st').textContent=p+'%';}};"
        "xhr.onload=()=>{if(xhr.status==200){document.getElementById('st').textContent='OK! Restarter...';setTimeout(()=>location.href='/',5000);}else{document.getElementById('st').textContent='Feil: '+xhr.responseText;}};"
        "xhr.onerror=()=>{document.getElementById('st').textContent='Nettverksfeil';};"
        "xhr.send(f);};"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    char buf[1024];
    int received, remaining = req->content_len;
    bool first_chunk = true;
    esp_err_t err;
    
    ESP_LOGI(TAG, "OTA: Starting, size=%d", remaining);
    web_log_add("OTA: Starting, %d bytes", remaining);
    
    while (remaining > 0) {
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA: Receive error");
            if (ota_handle) esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        
        if (first_chunk) {
            first_chunk = false;
            ota_partition = esp_ota_get_next_update_partition(NULL);
            if (!ota_partition) {
                ESP_LOGE(TAG, "OTA: No partition");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "OTA: Writing to %s", ota_partition->label);
            err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA: Begin failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                return ESP_FAIL;
            }
        }
        
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: Write failed");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }
    
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: End failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }
    
    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: Set boot failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA: Success! Restarting...");
    web_log_add("OTA: Success! Rebooting...");
    httpd_resp_sendstr(req, "OK");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t h = {"/", HTTP_GET, root_handler, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/ota", HTTP_GET, ota_page_handler, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/ota", HTTP_POST, ota_upload_handler, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/config", HTTP_GET, config_handler, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/config", HTTP_POST, api_config_post, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/status", HTTP_GET, api_status, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/debug", HTTP_GET, api_debug, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/disconnect", HTTP_GET, api_disconnect, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/mode/eco", HTTP_GET, api_eco, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/mode/tour", HTTP_GET, api_tour, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/mode/boost", HTTP_GET, api_boost, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/rock/start", HTTP_GET, api_rock_start, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/rock/continuous", HTTP_GET, api_rock_continuous, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/rock/stop", HTTP_GET, api_rock_stop, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/rock/autorenew", HTTP_GET, api_rock_autorenew, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/rescan", HTTP_GET, api_rescan, NULL};
        httpd_register_uri_handler(server, &h);
        h = (httpd_uri_t){"/api/log", HTTP_GET, api_log, NULL};
        httpd_register_uri_handler(server, &h);
        ESP_LOGI(TAG, "HTTP started");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "E-Priam Bridge v2.1 + MQTT");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    load_entity_names();
    wifi_init();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    start_webserver();
    mqtt_init();
    ble_init();
    
    // Start auto-renew monitoring task
    xTaskCreate(auto_renew_task, "autorenew", 2048, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Ready!");
}
