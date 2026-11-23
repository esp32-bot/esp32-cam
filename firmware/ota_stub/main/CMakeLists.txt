idf_component_register(
    SRCS "stub_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash app_update esp_driver_gpio lwip
)
