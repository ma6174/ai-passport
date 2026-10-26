idf_component_register(
    SRCS "main.c"
         "app_state.c"
         "app_ui.c"
         "audio_coordinator.c"
         "linx_ca.c"
         "linx_client.c"
         "linx_ws.c"
         "sonar_client.c"
         "sonar_config.c"
         "sonar_decision.c"
         "sonar_geo.c"
         "sonar_http.c"
         "sonar_net.c"
         "sonar_policy.c"
         "sonar_store.c"
         "sonar_time.c"
         "sonar_uri.c"
         "sonar_wav.c"
         "sonar_session.c"
         "sonar_text.c"
         "font_cjk16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp-tls mbedtls json spiffs
             esp_app_format driver
    EMBED_FILES "assets/prompt.wav" "assets/beep.wav"
)
