idf_component_register(SRCS "ptp_net.c" "ptp_frame.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_eth esp_netif
                       PRIV_REQUIRES lwip freertos)
