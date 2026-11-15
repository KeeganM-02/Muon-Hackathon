idf_component_register(
    SRCS "main.c" "wifi.c" "net.c" "link.c" "api.c" "jhelp.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_driver_uart esp_driver_gpio
)
