idf_component_register(SRCS "msg_bus.c" "main.c" "speech_recognition.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_websocket_client json
                             driver esp-sr)