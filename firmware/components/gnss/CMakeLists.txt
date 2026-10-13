idf_component_register(SRCS "gnss.c" "nmea.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart freertos)
