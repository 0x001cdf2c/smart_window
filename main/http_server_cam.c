#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "http_server_cam.h"
#include "camera_capture.h"

static const char *TAG = "HTTP_CAM";

#define JPG_BUF_SIZE (256 * 1024)

static httpd_handle_t s_server = NULL;

static const char INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Camera</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box;}"
    "body{font-family:system-ui,sans-serif;background:#111;color:#fff;text-align:center;}"
    "h1{font-size:16px;padding:12px;color:#aaa;}"
    "img{max-width:100%;border-radius:4px;}"
    ".wrap{margin:8px;}"
    "button{padding:10px 24px;font-size:14px;background:#2563eb;color:#fff;border:none;"
    "border-radius:6px;cursor:pointer;margin:8px;}"
    "button:hover{background:#3b82f6;}"
    "</style></head><body>"
    "<h1>ESP32-P4 Camera</h1>"
    "<div class=\"wrap\"><img id=\"live\" src=\"/frame?init=1\"></div>"
    "<button onclick=\"snap()\">Take Snapshot</button>"
    "<div class=\"wrap\"><img id=\"snap\" style=\"display:none\"></div>"
    "<script>"
    "setInterval(function(){"
    "document.getElementById('live').src='/frame?t='+Date.now();"
    "},1000);"
    "function snap(){"
    "var s=document.getElementById('snap');"
    "s.src='/frame?snap='+Date.now();"
    "s.style.display='block';"
    "}"
    "</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t frame_handler(httpd_req_t *req)
{
    static uint8_t *jpg_buf = NULL;
    static int req_count = 0;
    req_count++;

    if (!jpg_buf) {
        jpg_buf = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpg_buf) jpg_buf = malloc(JPG_BUF_SIZE);
        if (!jpg_buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    size_t out_len = 0;
    esp_err_t ret = camera_get_jpg_copy(jpg_buf, JPG_BUF_SIZE, &out_len);
    if (ret != ESP_OK || out_len == 0) {
        if (req_count <= 5) {
            ESP_LOGW(TAG, "/frame req #%d: no JPEG frame yet (ret=%s, out_len=%u)",
                     req_count, esp_err_to_name(ret), (unsigned)out_len);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame");
        return ESP_FAIL;
    }

    if (req_count <= 5 || req_count % 100 == 0) {
        ESP_LOGI(TAG, "/frame req #%d: OK %u bytes", req_count, (unsigned)out_len);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, (const char *)jpg_buf, out_len);
}

/* MJPEG stream (alternative, not used by default web page) */
#define MJPEG_BOUNDARY "123456789000000000000987654321"

static esp_err_t stream_handler(httpd_req_t *req)
{
    uint8_t *jpg_buf = heap_caps_malloc(JPG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) jpg_buf = malloc(JPG_BUF_SIZE);
    if (!jpg_buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    esp_err_t ret = httpd_resp_set_type(req,
        "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY);
    if (ret != ESP_OK) { free(jpg_buf); return ret; }

    char hdr[256];
    while (1) {
        size_t out_len = 0;
        ret = camera_get_jpg_copy(jpg_buf, JPG_BUF_SIZE, &out_len);
        if (ret != ESP_OK || out_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        int hdr_len = snprintf(hdr, sizeof(hdr),
            "\r\n--" MJPEG_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n", (unsigned)out_len);
        if (httpd_resp_send_chunk(req, hdr, hdr_len) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char *)jpg_buf, out_len) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    free(jpg_buf);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const char *json = "{\"camera\":\"running\",\"width\":1280,\"height\":720}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t http_server_cam_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        s_server = NULL;
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
    httpd_uri_t uri_frame = { .uri = "/frame", .method = HTTP_GET, .handler = frame_handler, .user_ctx = NULL };
    httpd_uri_t uri_capture = { .uri = "/capture", .method = HTTP_GET, .handler = frame_handler, .user_ctx = NULL };
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_frame);
    httpd_register_uri_handler(s_server, &uri_capture);
    httpd_register_uri_handler(s_server, &uri_stream);
    httpd_register_uri_handler(s_server, &uri_status);

    ESP_LOGI(TAG, "HTTP camera server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t http_server_cam_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP camera server stopped");
    }
    return ESP_OK;
}
