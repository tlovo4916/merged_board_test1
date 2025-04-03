idf_component_register(SRCS "main.c" "board.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_wifi nvs_flash esp_http_server esp_websocket_client es8311 es7210 json
                    PRIV_INCLUDE_DIRS "/Users/tlovo/esp/v5.3.2/esp-idf/components/json/cJSON"
                    EMBED_FILES "index.html" "pcm/1.pcm" "pcm/2.pcm" "pcm/3.pcm" "pcm/4.pcm") 