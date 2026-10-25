# The secure element and the security state of the node (M6).
#   src/  pure C: the ATECC608B configuration FireX writes, the lock CRC and the provisioning
#         steps (host-tested by firmware/Makefile, test/test_secure.c)
#   hw/   the ATECC608B over cryptoauthlib, and the eFuse state (secure boot, flash
#         encryption, debug interfaces, ROM loader, NVS encryption)
# esp-cryptoauthlib comes from idf_component.yml. It is built into every image; only an
# image with CONFIG_FIREX_KEY_ATECC608B calls it.
idf_component_register(
    SRCS
        "src/se_config.c"
        "hw/atecc.c"
        "hw/security_status.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        bootloader_support
        efuse
        freertos
        log
        spi_flash
)
