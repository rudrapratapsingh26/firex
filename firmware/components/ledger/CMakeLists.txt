# The FireX ledger on the node.
#   src/  pure C: SHA-256, canonical CBOR, entries, records, ring buffer, chain writer
#         (host-tested by firmware/Makefile against ledger/vectors)
#   hw/   LittleFS segment store and the device key: mbedTLS P-256 in NVS on the dev kit,
#         the ATECC608B on the EVT board (CONFIG_FIREX_KEY_ATECC608B, components/secure)
idf_component_register(
    SRCS
        "src/cbor.c"
        "src/entry.c"
        "src/ledger.c"
        "src/record.c"
        "src/ring.c"
        "src/sha256.c"
        "hw/device_key.c"
        "hw/store_littlefs.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mbedtls
        nvs_flash
        bootloader_support
        esp_hw_support
        secure
        vfs
)
