#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"

#include "wifi_http_server.h"
#include "camera_snapshot.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define EXAMPLE_ESP_WIFI_SSID      "iPhone"
#define EXAMPLE_ESP_WIFI_PASS      "happychy"
#define EXAMPLE_ESP_MAXIMUM_RETRY  5

#define JPG_COPY_SIZE   (512 * 1024)

static const char *TAG = "CAM_WIFI_HTTP";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static httpd_handle_t s_http_server = NULL;

/*
 * 说明：
 * 这里不再让网页默认使用 /stream 长连接。
 * Live Preview 改为定时请求 /frame。
 * Snapshot 点击时也请求 /frame。
 * 这样每次 HTTP 请求发完就结束，不会长期占用 HTTP server。
 */
static const char *INDEX_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<title>ESP32-P4 Camera Web</title>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#f5f5f5;}"
    ".row{display:flex;gap:20px;flex-wrap:wrap;max-width:1700px;}"
    ".col{flex:1;min-width:400px;}"
    ".card{background:white;padding:15px;border-radius:12px;"
    "box-shadow:0 2px 10px rgba(0,0,0,0.12);margin-bottom:15px;}"
    "h1{color:#1f4e79;}"
    "h2{color:#1f4e79;margin:0 0 10px 0;font-size:18px;}"
    "img{width:100%;border:1px solid #ccc;border-radius:8px;background:#ddd;}"
    "button{padding:12px 28px;font-size:16px;background:#1f4e79;color:white;"
    "border:none;border-radius:6px;cursor:pointer;margin:10px 0;display:block;}"
    "button:hover{background:#2a6ba0;}"
    "button:active{background:#153d5e;}"
    ".ok{color:green;font-weight:bold;margin:0 0 10px 0;}"
    ".snapshot-msg{color:#666;font-size:14px;margin-top:8px;}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>ESP32-P4 Camera Web</h1>"
    "<p class=\"ok\">WiFi, HTTP server and camera are running.</p>"
    "<button onclick=\"takeSnapshot()\">Capture Snapshot</button>"

    "<div class=\"row\">"

    "<div class=\"col\">"
    "<div class=\"card\">"
    "<h2>Live Preview</h2>"
    "<img id=\"live\" src=\"/frame?init=1\">"
    "<p class=\"snapshot-msg\">Live preview refreshes by requesting /frame.</p>"
    "</div>"
    "</div>"

    "<div class=\"col\">"
    "<div class=\"card\">"
    "<h2>Snapshot</h2>"
    "<img id=\"snap\" src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\">"
    "<p id=\"snap-msg\" class=\"snapshot-msg\">Press the button to take a snapshot.</p>"
    "</div>"
    "</div>"

    "</div>"

    "<script>"
    "function refreshLive(){"
    "var img=document.getElementById('live');"
    "img.src='/frame?live='+Date.now();"
    "}"
    "setInterval(refreshLive,1000);"

    "function takeSnapshot(){"
    "var ts=Date.now();"
    "var s=document.getElementById('snap');"
    "var m=document.getElementById('snap-msg');"
    "s.onload=function(){"
    "m.textContent='Snapshot taken at '+new Date(ts).toLocaleTimeString();"
    "};"
    "s.onerror=function(){"
    "m.textContent='Snapshot failed at '+new Date(ts).toLocaleTimeString();"
    "};"
    "s.src='/frame?snap='+ts;"
    "}"
    "</script>"

    "</body>"
    "</html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const char *json =
        "{"
            "\"device\":\"ESP32-P4\","
            "\"wifi\":\"connected\","
            "\"http\":\"running\","
            "\"camera\":\"running\","
            "\"preview\":\"frame-refresh\""
        "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/*
 * frame_get_handler:
 * 返回一张当前缓存 JPEG。
 * /frame、/capture 都可以共用这个函数。
 */
static esp_err_t frame_get_handler(httpd_req_t *req)
{
    static uint8_t *jpg_buf = NULL;

    if (jpg_buf == NULL) {
        jpg_buf = heap_caps_malloc(JPG_COPY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (jpg_buf == NULL) {
            jpg_buf = malloc(JPG_COPY_SIZE);
        }

        if (jpg_buf == NULL) {
            ESP_LOGE(TAG, "frame: jpg_buf alloc failed");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "frame: jpg_buf allocated, size=%u", (unsigned)JPG_COPY_SIZE);
    }

    size_t out_len = 0;
    esp_err_t ret = camera_get_jpg_copy(jpg_buf, JPG_COPY_SIZE, &out_len);

    if (ret != ESP_OK || out_len == 0) {
        ESP_LOGW(TAG, "frame: camera_get_jpg_copy failed, ret=%d, len=%u",
                 ret, (unsigned)out_len);

        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no camera frame");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    ret = httpd_resp_send(req, (const char *)jpg_buf, out_len);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "frame: send failed, ret=%d", ret);
    }

    return ret;
}

/*
 * 备用 MJPEG stream。
 * 当前网页默认不使用它。
 * 如果手动访问 /stream，可以测试真正 MJPEG 长连接。
 */
#define MJPEG_BOUNDARY  "123456789000000000000987654321"

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    uint8_t *jpg_buf = heap_caps_malloc(JPG_COPY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpg_buf == NULL) {
        jpg_buf = malloc(JPG_COPY_SIZE);
    }

    if (jpg_buf == NULL) {
        ESP_LOGE(TAG, "stream: jpg_buf alloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_set_type(
        req,
        "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY
    );

    if (ret != ESP_OK) {
        heap_caps_free(jpg_buf);
        return ret;
    }

    char hdr_buf[256];

    while (1) {
        size_t out_len = 0;

        ret = camera_get_jpg_copy(jpg_buf, JPG_COPY_SIZE, &out_len);
        if (ret != ESP_OK || out_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        int hdr_len = snprintf(
            hdr_buf,
            sizeof(hdr_buf),
            "\r\n--" MJPEG_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            (unsigned)out_len
        );

        if (httpd_resp_send_chunk(req, hdr_buf, hdr_len) != ESP_OK) {
            break;
        }

        if (httpd_resp_send_chunk(req, (const char *)jpg_buf, out_len) != ESP_OK) {
            break;
        }

        /*
         * 限制帧率，避免 WiFi/HTTP 发送缓冲区被打满。
         * 80ms 大约 12.5fps。
         */
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    heap_caps_free(jpg_buf);
    ESP_LOGI(TAG, "stream: client disconnected");
    return ESP_OK;
}

static void start_webserver(void)
{
    if (s_http_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /*
     * 稍微增大 URI handler 数量，避免注册 /frame、/capture、/stream 时不够。
     */
    config.max_uri_handlers = 8;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&s_http_server, &config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        s_http_server = NULL;
        return;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t frame_uri = {
        .uri = "/frame",
        .method = HTTP_GET,
        .handler = frame_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = frame_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &frame_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &capture_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &stream_uri));

    ESP_LOGI(TAG, "HTTP server started");
}

static void stop_webserver(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, connecting...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        stop_webserver();

        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to AP, retry=%d", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

        ESP_LOGW(TAG, "Connect to AP failed");

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        start_webserver();
    }
}

void wifi_http_server_start(void)
{
    ESP_LOGI(TAG, "WiFi HTTP module starting");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &instance_any_id
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &instance_got_ip
        )
    );

    wifi_config_t wifi_config = {0};

    strncpy(
        (char *)wifi_config.sta.ssid,
        EXAMPLE_ESP_WIFI_SSID,
        sizeof(wifi_config.sta.ssid) - 1
    );

    strncpy(
        (char *)wifi_config.sta.password,
        EXAMPLE_ESP_WIFI_PASS,
        sizeof(wifi_config.sta.password) - 1
    );

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished");
    ESP_LOGI(TAG, "Connecting to SSID: %s", EXAMPLE_ESP_WIFI_SSID);
}