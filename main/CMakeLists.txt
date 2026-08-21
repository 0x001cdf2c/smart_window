idf_component_register(SRCS "adaptive_mode.c" "local_nlp.c" "rain_sensor.c" "adc_ads1115.c" "control_algorithm.c" "sht3x.c" "bh1750.c" "smoke_sensor.c" "airflow_sensor.c" "wind_scanner.c""msg_bus.c" "main.c" "speech_recognition.c" "voice_reply.c" "voice_data_xiaole.c" "servo_manager.c" "display.c" "ui.c" "camera_capture.c" "http_server_cam.c" "i2c_bus.c" "soft_i2c.c" "lv_font_cn_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_websocket_client json esp_adc
                             driver esp-sr esp_partition app_update bootloader_support esp_hosted
                             esp_lcd esp_lcd_ek79007 lvgl esp_video esp_cam_sensor example_video_common
                             esp_http_server esp_driver_jpeg)
