// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/apps/sntp.h"
#include "i2c_bus.h"
#include "driver/rmt.h"
#include "led_strip.h"
#include "touch.h"
#include "audio.h"
#include "es8311.h"
#include "board.h"

#include "cam.h"
#include "lcd.h"
#include "ov2640.h"
#include "ov3660.h"
#include "sensor.h"

static const char *TAG = "main";

uint8_t mac[16];
#define CAM_WIDTH   (320)
#define CAM_HIGH    (240)
#define JPEG_INIT  0
// #define CAM_RESET 2
// #define CAM_PWDN 1

#ifdef CONFIG_KALUGA_WIFI
static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;
static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid, CONFIG_WIFI_SSID, strlen(CONFIG_WIFI_SSID) + 1);
    memcpy(wifi_config.sta.password, CONFIG_WIFI_PASSWORD, strlen(CONFIG_WIFI_PASSWORD) + 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

#endif



static void cam_task(void *arg)
{

    lcd_config_t lcd_config = {
#ifdef CONFIG_LCD_ST7789
        .clk_fre         = 80 * 1000 * 1000, /*!< ILI9341 Stable frequency configuration */
#endif
#ifdef CONFIG_LCD_ILI9341
        .clk_fre         = 40 * 1000 * 1000, /*!< ILI9341 Stable frequency configuration */
#endif
        .pin_clk         = LCD_CLK,
        .pin_mosi        = LCD_MOSI,
        .pin_dc          = LCD_DC,
        .pin_cs          = LCD_CS,
        .pin_rst         = LCD_RST,
        .pin_bk          = LCD_BK,
        .max_buffer_size = 2 * 1024,
        .horizontal      = 2 /*!< 2: UP, 3: DOWN */
    };

    lcd_init(&lcd_config);

    cam_config_t cam_config = {
        .bit_width    = 8,
#if JPEG_INIT
        .mode.jpeg    = 1,
#else
        .mode.jpeg    = 0,
#endif
        .xclk_fre     = 10 * 1000 * 1000,
        .pin  = {
            .xclk     = CAM_XCLK,
            .pclk     = CAM_PCLK,
            .vsync    = CAM_VSYNC,
            .hsync    = CAM_HSYNC,
        },
        .pin_data     = {CAM_D0, CAM_D1, CAM_D2, CAM_D3, CAM_D4, CAM_D5, CAM_D6, CAM_D7},
        .vsync_invert = true,
        .hsync_invert = false,
        .size = {
            .width    = CAM_WIDTH,
            .high     = CAM_HIGH,
        },
        .max_buffer_size = 8 * 1024,
        .task_stack      = 1024,
        .task_pri        = 3
    };

    /*!< With PingPang buffers, the frame rate is higher, or you can use a separate buffer to save memory */
    cam_config.frame1_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    cam_config.frame2_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);

    cam_init(&cam_config);

    sensor_t sensor;
    int camera_version = 0;      /*!<If the camera version is determined, it can be set to manual mode */
    // SCCB_Init(CAM_SDA, CAM_SCL);
    sensor.slv_addr = SCCB_Probe();
    ESP_LOGI(TAG, "sensor_id: 0x%x\n", sensor.slv_addr);

#ifdef CONFIG_CAMERA_OV2640
    camera_version = 2640;
#endif
#ifdef CONFIG_CAMERA_OV3660
    camera_version = 3660;
#endif
#ifdef CONFIG_CAMERA_AUTO
    /*!< If you choose this mode, Dont insert the Audio board, audio will affect the camera register read. */
#endif

    if (sensor.slv_addr == 0x30 || camera_version == 2640) { /*!< Camera: OV2640 */
        ESP_LOGI(TAG, "OV2640 init start...");

        if (OV2640_Init(0, 1) != 0) {
            goto fail;
        }

        if (cam_config.mode.jpeg) {
            OV2640_JPEG_Mode();
        } else {
            OV2640_RGB565_Mode(false);	/*!< RGB565 mode */
        }

        OV2640_ImageSize_Set(800, 600);
        OV2640_ImageWin_Set(0, 0, 800, 600);
        OV2640_OutSize_Set(CAM_WIDTH, CAM_HIGH);
    } else if (sensor.slv_addr == 0x3C || camera_version == 3660) { /*!< Camera: OV3660 */

        sensor.slv_addr = 0x3C; /*!< In special cases, slv_addr may change */
        ESP_LOGI(TAG, "OV3660 init start...");
        ov3660_init(&sensor);
        sensor.init_status(&sensor);

        if (sensor.reset(&sensor) != 0) {
            goto fail;
        }

        if (cam_config.mode.jpeg) {
            sensor.set_pixformat(&sensor, PIXFORMAT_JPEG);
        } else {
            sensor.set_pixformat(&sensor, PIXFORMAT_RGB565);
        }

        /*!< TotalX gets smaller, frame rate goes up */
        /*!< TotalY gets smaller, frame rate goes up, vsync gets shorter */
        sensor.set_res_raw(&sensor, 0, 0, 2079, 1547, 8, 2, 1920, 800, CAM_WIDTH, CAM_HIGH, true, true);
        sensor.set_vflip(&sensor, 1);
        sensor.set_hmirror(&sensor, 1);
        sensor.set_pll(&sensor, false, 15, 1, 0, false, 0, true, 5); /*!< ov3660: 39 fps */
    } else {
        ESP_LOGE(TAG, "sensor is temporarily not supported\n");
        goto fail;
    }

    ESP_LOGI(TAG, "camera init done\n");
    cam_start();

    int cnt = 50;

    while (cnt--) {
        
        uint8_t *cam_buf = NULL;
        cam_take(&cam_buf);

#if JPEG_INIT

        int w, h;
        uint8_t *img = jpeg_decode(cam_buf, &w, &h);

        if (img) {
            ESP_LOGI(TAG, "jpeg: w: %d, h: %d\n", w, h);
            lcd_set_index(0, 0, w - 1, h - 1);
            lcd_write_data(img, w * h * sizeof(uint16_t));
            free(img);
        }

#else
        lcd_set_index(0, 0, CAM_WIDTH - 1, CAM_HIGH - 1);
        lcd_write_data(cam_buf, CAM_WIDTH * CAM_HIGH * 2);
#endif
        cam_give(cam_buf);
        /*!< Use a logic analyzer to observe the frame rate */
        // gpio_set_level(LCD_BK, 1);
        // gpio_set_level(LCD_BK, 0);
    }


fail:
    free(cam_config.frame1_buffer);
    free(cam_config.frame2_buffer);
    cam_deinit();
    // vTaskDelete(NULL);
}
esp_err_t spiffs_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    /*!< Use settings defined above to initialize and mount SPIFFS filesystem. */
    /*!< Note: esp_vfs_spiffs_register is an all-in-one convenience function. */
    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }

        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    /*!< Open renamed file for reading */
    ESP_LOGI(TAG, "Reading file");
    FILE *f = fopen("/spiffs/spiffs.txt", "r");

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }

    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    /*!< strip newline */
    char *pos = strchr(line, '\n');

    if (pos) {
        *pos = '\0';
    }

    ESP_LOGI(TAG, "Read from file: '%s'", line);

    return ESP_OK;
}

void app_main()
{
    /*!< Print basic information */
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    /*!< Initialize NVS */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(spiffs_init());

    ESP_ERROR_CHECK(i2c_bus_init());

    while (1) {
        cam_task(NULL);
        vTaskDelay(1000 / portTICK_RATE_MS);
        // cam_task(NULL);
        audio_init();

        vTaskDelay(5000 / portTICK_RATE_MS);
        audio_deinit();
        vTaskDelay(1000 / portTICK_RATE_MS);
    }


}
