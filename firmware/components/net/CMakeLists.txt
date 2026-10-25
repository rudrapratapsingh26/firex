# The node's link to the site gateway (M3).
#   src/  pure C: pairing codes, the provisioning exchange, store-and-forward sync
#         (host-tested by firmware/Makefile, test/test_net.c)
#   hw/   ESP-IDF: the TLS key, network settings in NVS, Wi-Fi onboarding, the provisioning client
idf_component_register(
    SRCS
        "src/prov.c"
        "src/sync.c"
        "hw/net_store.c"
        "hw/prov_client.c"
        "hw/tls_key.c"
        "hw/wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ledger
    PRIV_REQUIRES
        bootloader_support
        esp_event
        esp_http_client
        esp_hw_support
        esp_netif
        esp_wifi
        json
        mbedtls
        nvs_flash
        wifi_provisioning
)
