idf_component_register(SRCS "esp_ot_cli.c" "coap_demo.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_netif nvs_flash openthread vfs
                                ot_examples_common ot_led
                                espressif__esp_ot_cli_extension coap)