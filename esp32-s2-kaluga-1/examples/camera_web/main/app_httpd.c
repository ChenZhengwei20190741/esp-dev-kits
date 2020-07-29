// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
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
#include "app_httpd.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cam.h"
#include "board.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "esp_log.h"

extern const unsigned char index_setting_page_html_gz_start[] asm("_binary_settingPage_html_gz_start");
extern const unsigned char index_setting_page_html_gz_end[]   asm("_binary_settingPage_html_gz_end");

extern const unsigned char index_setting_succ_html_gz_start[] asm("_binary_SaveResponseSucc_html_gz_start");
extern const unsigned char index_setting_succ_html_gz_end[]   asm("_binary_SaveResponseSucc_html_gz_end");

extern const unsigned char index_setting_error_html_gz_start[] asm("_binary_SaveResponseError_html_gz_start");
extern const unsigned char index_setting_error_html_gz_end[]   asm("_binary_SaveResponseError_html_gz_end");

static const char *TAG = "ap_config";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

static esp_err_t setting_handler(httpd_req_t *req)
{
    char  *buf;
    size_t buf_len;
    char ssid_value[32] = {0,};
    char pass_value[32] = {0,};
    size_t index_setting_succ_html_gz_len = index_setting_succ_html_gz_end - index_setting_succ_html_gz_start;
    size_t index_setting_error_html_gz_len = index_setting_error_html_gz_end - index_setting_error_html_gz_start;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    buf_len = httpd_req_get_url_query_len(req) + 1;

    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);

        if (!buf) {
            httpd_resp_send(req, (const char *)index_setting_error_html_gz_start, index_setting_error_html_gz_len);
            return ESP_FAIL;
        }

        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "SSID", ssid_value, sizeof(ssid_value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send(req, (const char *)index_setting_error_html_gz_start, index_setting_error_html_gz_len);
                return ESP_FAIL;
            }

            if (httpd_query_key_value(buf, "pass", pass_value, sizeof(pass_value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send(req, (const char *)index_setting_error_html_gz_start, index_setting_error_html_gz_len);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send(req, (const char *)index_setting_error_html_gz_start, index_setting_error_html_gz_len);
            return ESP_FAIL;
        }

        free(buf);
    } else {
        httpd_resp_send(req, (const char *)index_setting_error_html_gz_start, index_setting_error_html_gz_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSID = %s, pass = %s", ssid_value, pass_value);
    httpd_resp_send(req, (const char *)index_setting_succ_html_gz_start, index_setting_succ_html_gz_len);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    size_t index_setting_page_html_gz_len = index_setting_page_html_gz_end - index_setting_page_html_gz_start;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    httpd_resp_send(req, (const char *)index_setting_page_html_gz_start, index_setting_page_html_gz_len);
    return ESP_OK;
}


static esp_err_t stream_handler(httpd_req_t *req)
{
    // printf("----------stream_handler----start----------\n");
    struct timeval _timestamp;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[128];

    static int64_t last_frame = 0;

    if (!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    if (res != ESP_OK) {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");




    while (true) {
        _jpg_buf_len = cam_take(&_jpg_buf);


        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        if (res == ESP_OK) {
            uint64_t us = (uint64_t)esp_timer_get_time();
            _timestamp.tv_sec = us / 1000000UL;
            _timestamp.tv_usec = us % 1000000UL;
            size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
            // ESP_LOGI(TAG, "time:%lld", us);
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }

        cam_give(_jpg_buf);
        gpio_set_level(LCD_BK, 1);
        gpio_set_level(LCD_BK, 0);

        // if (res != ESP_OK) {
        //     break;
        // }


    }
        free(_jpg_buf);
        _jpg_buf = NULL;

    return res;
}
// static esp_err_t status_handler(httpd_req_t *req)
// {
//     static char json_response[1024];

//     sensor_t *s = esp_camera_sensor_get();
//     char *p = json_response;
//     *p++ = '{';

//     if(s->id.PID == OV5640_PID || s->id.PID == OV3660_PID){
//         for(int reg = 0x3400; reg < 0x3406; reg+=2){
//             p+=print_reg(p, s, reg, 0xFFF);//12 bit
//         }
//         p+=print_reg(p, s, 0x3406, 0xFF);

//         p+=print_reg(p, s, 0x3500, 0xFFFF0);//16 bit
//         p+=print_reg(p, s, 0x3503, 0xFF);
//         p+=print_reg(p, s, 0x350a, 0x3FF);//10 bit
//         p+=print_reg(p, s, 0x350c, 0xFFFF);//16 bit

//         for(int reg = 0x5480; reg <= 0x5490; reg++){
//             p+=print_reg(p, s, reg, 0xFF);
//         }

//         for(int reg = 0x5380; reg <= 0x538b; reg++){
//             p+=print_reg(p, s, reg, 0xFF);
//         }

//         for(int reg = 0x5580; reg < 0x558a; reg++){
//             p+=print_reg(p, s, reg, 0xFF);
//         }
//         p+=print_reg(p, s, 0x558a, 0x1FF);//9 bit
//     } else {
//         p+=print_reg(p, s, 0xd3, 0xFF);
//         p+=print_reg(p, s, 0x111, 0xFF);
//         p+=print_reg(p, s, 0x132, 0xFF);
//     }

//     p += sprintf(p, "\"board\":\"%s\",", CAM_BOARD);
//     p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
//     p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
//     p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
//     p += sprintf(p, "\"quality\":%u,", s->status.quality);
//     p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
//     p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
//     p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
//     p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
//     p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
//     p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
//     p += sprintf(p, "\"awb\":%u,", s->status.awb);
//     p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
//     p += sprintf(p, "\"aec\":%u,", s->status.aec);
//     p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
//     p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
//     p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
//     p += sprintf(p, "\"agc\":%u,", s->status.agc);
//     p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
//     p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
//     p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
//     p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
//     p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
//     p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
//     p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
//     p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
//     p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);

//     p += sprintf(p, ",\"led_intensity\":%d", -1);
//     *p++ = '}';
//     *p++ = 0;
//     httpd_resp_set_type(req, "application/json");
//     httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
//     return httpd_resp_send(req, json_response, strlen(json_response));
// }
void app_httpd_main()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t setting_uri = {
        .uri       = "/setting",
        .method    = HTTP_GET,
        .handler   = setting_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

        // httpd_uri_t status_uri = {
        // .uri = "/status",
        // .method = HTTP_GET,
        // .handler = status_handler,
        // .user_ctx = NULL};

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        // httpd_register_uri_handler(camera_httpd, &setting_uri);
        // httpd_register_uri_handler(camera_httpd, &status_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
