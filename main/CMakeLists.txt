idf_component_register(SRCS "msg_bus.c" "main.c" "speech_recognition.c" "voice_reply.c" "voice_data_xiaole.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_websocket_client json
                             driver esp-sr esp_partition app_update bootloader_support esp_hosted)