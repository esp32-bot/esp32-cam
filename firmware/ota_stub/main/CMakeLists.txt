idf_component_register(
    SRCS "stub_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash app_update bootloader_support
             esp_partition esp_timer esp_driver_gpio lwip
)
