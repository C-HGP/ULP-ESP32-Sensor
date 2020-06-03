/*

blabla


*/

// Libraaaaaaaaaaaaaaaaaaaaaaries
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <time.h>
#include "cJSON.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "driver/touch_pad.h"
#include "ds18b20.h"
#include "esp32/ulp.h"
#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "mqtt_config.h"
#include "nvs_flash.h"
#include "owb.h"
#include "owb_rmt.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/sens_reg.h"
#include "wifi_manager.h"

// Defines for tempread
#define GPIO_DS18B20_0 (CONFIG_ONE_WIRE_GPIO)
#define MAX_DEVICES (8)
#define DS18B20_RESOLUTION (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD (5000)  // milliseconds

// Global Variable declare
static RTC_DATA_ATTR struct timeval sleep_enter_time;
static const char TAG[] = "main";
char line[64];
float readings[MAX_DEVICES] = {0};
char mqttMessage[64];

// Function declare
void INIT_spiff();
void WRITEFile();
void READFile();
void TEMPCollect();
void ESPDeepSleep();

// Print out once connected then start tasks
/*
- Initiate SPIFFs filesystem
- Write to SPIFFs file
- Read SPIFFs file and save it in "line"
- Unmount SPIFFs
- Collect temprature and start MQTT service
*/
void cb_connection_ok(void* pvParameter) {
    ESP_LOGI(TAG, "====I have a connection!====");
    INIT_spiff();
    WRITEFile();
    READFile();
    esp_vfs_spiffs_unregister(NULL);
    ESP_LOGI(TAG, "SPIFFS unmounted");
    TEMPCollect();
}

// MQTT Print events and send MQTT
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "Connected",
                                             0, 1, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos0");
            ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", mqttMessage,
                                             0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

// Start MQTT and define server, this one uses LINE which is
// collected from SPIFFs
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = line,
        .event_handle = mqtt_event_handler,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

// Mount spiff file system
void INIT_spiff() {
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                  .partition_label = NULL,
                                  .max_files = 5,
                                  .format_if_mount_failed = true};

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)",
                     esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}
// Write to spiffs hello example
void WRITEFile() {
    // Use POSIX and C standard library functions to work with files.
    // First create a file.
    ESP_LOGI(TAG, "Opening file");
    FILE* f = fopen("/spiffs/hello.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    fprintf(f, "mqtt://192.168.105.88");
    fclose(f);
    ESP_LOGI(TAG, "File written");
}
// Read and store whats inside the textfile
void READFile() {
    FILE* f = fopen("/spiffs/hello.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char* pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);
}

// Scan for TEMPSENSOR and print
void TEMPCollect() {
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus, using the RMT timeslot driver
    OneWireBus* owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1,
                             RMT_CHANNEL_0);
    owb_use_crc(owb, true);  // enable CRC check for ROM code

    // Find all connected devices
    printf("Find devices:\n");
    OneWireBus_ROMCode device_rom_codes[MAX_DEVICES] = {0};
    int num_devices = 0;
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found) {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s,
                                 sizeof(rom_code_s));
        printf("  %d : %s\n", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    printf("Found %d device%s\n", num_devices, num_devices == 1 ? "" : "s");

    // In this example, if a single device is present, then the ROM code is
    // probably not very interesting, so just print it out. If there are
    // multiple devices, then it may be useful to check that a specific device
    // is present.

    if (num_devices == 1) {
        // For a single device only:
        OneWireBus_ROMCode rom_code;
        owb_status status = owb_read_rom(owb, &rom_code);
        if (status == OWB_STATUS_OK) {
            char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
            owb_string_from_rom_code(rom_code, rom_code_s, sizeof(rom_code_s));
            printf("Single device %s present\n", rom_code_s);
        } else {
            printf("An error occurred reading ROM code: %d", status);
        }
    } else {
        // Search for a known ROM code (LSB first):
        // For example: 0x1502162ca5b2ee28
        OneWireBus_ROMCode known_device = {
            .fields.family = {0x28},
            .fields.serial_number = {0xee, 0xb2, 0xa5, 0x2c, 0x16, 0x02},
            .fields.crc = {0x15},
        };
        char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
        owb_string_from_rom_code(known_device, rom_code_s, sizeof(rom_code_s));
        bool is_present = false;

        owb_status search_status =
            owb_verify_rom(owb, known_device, &is_present);
        if (search_status == OWB_STATUS_OK) {
            printf("Device %s is %s\n", rom_code_s,
                   is_present ? "present" : "not present");
        } else {
            printf("An error occurred searching for known device: %d",
                   search_status);
        }
    }

    // Create DS18B20 devices on the 1-Wire bus
    DS18B20_Info* devices[MAX_DEVICES] = {0};
    for (int i = 0; i < num_devices; ++i) {
        DS18B20_Info* ds18b20_info = ds18b20_malloc();  // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1) {
            printf("Single device optimisations enabled\n");
            ds18b20_init_solo(ds18b20_info, owb);  // only one device on bus
        } else {
            ds18b20_init(ds18b20_info, owb,
                         device_rom_codes[i]);  // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info,
                        true);  // enable CRC check for temperature readings
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }
    // Read temperatures more efficiently by starting conversions on all devices
    // at the same time
    int errors_count[MAX_DEVICES] = {0};
    int sample_count = 0;
    if (num_devices > 0) {
        TickType_t last_wake_time = xTaskGetTickCount();

        while (1) {
            last_wake_time = xTaskGetTickCount();

            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may
            // fail (using printf before reading may take too long)

            DS18B20_ERROR errors[MAX_DEVICES] = {0};

            for (int i = 0; i < num_devices; ++i) {
                errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
            }

            // Print results in a separate loop, after all have been read
            printf("\nTemperature readings (degrees C): sample %d\n",
                   ++sample_count);
            for (int i = 0; i < num_devices; ++i) {
                if (errors[i] != DS18B20_OK) {
                    ++errors_count[i];
                }

                printf("  %d: %.2f    %d errors\n", i, readings[i],
                       errors_count[i]);
                snprintf(mqttMessage, sizeof mqttMessage, "%.2f", readings[i]);
                // After extracted data from tempsensor, start the MQTT service
                // to send to broker
                mqtt_app_start();
                // Sleep/Hibernate after MQTT task
                ESPDeepSleep();
            }
        }
    }
}
// DeepSleep function for ESP
void ESPDeepSleep() {
    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 +
                        (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: {
            printf("Wake up from timer. Time spent in deep sleep: %dms\n",
                   sleep_time_ms);
            break;
        }
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            printf("Not a deep sleep reset\n");
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    // wakeup_time_sec controles amount of time spent in sleep
    const int wakeup_time_sec = 20;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);

    printf("Entering deep sleep\n");
    gettimeofday(&sleep_enter_time, NULL);

    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    esp_wifi_stop();
    esp_deep_sleep_start();
}

// Main function
void app_main() {
    // Start main function
    wifi_manager_start();

    // When WiFi has tried connecting and recivied ok, enter this to start
    // procedures
    wifi_manager_set_callback(EVENT_STA_GOT_IP, &cb_connection_ok);

    // i'm still alive
    ESP_LOGI(TAG, "This was a triumph...");
}