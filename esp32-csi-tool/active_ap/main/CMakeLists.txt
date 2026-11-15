idf_component_register(
        SRCS main.cc
        PRIV_REQUIRES spi_flash esp_wifi esp_http_server nvs_flash fatfs esp_hw_support
        INCLUDE_DIRS "")
