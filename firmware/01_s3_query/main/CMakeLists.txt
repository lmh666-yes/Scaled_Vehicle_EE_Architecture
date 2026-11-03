idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_tsens
                                esp_partition
                                spi_flash
                                esp_hw_support
                                esp_timer
                                esp_app_format
                                esp_common
                                esp_psram)
