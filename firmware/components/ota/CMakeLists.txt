# Over-the-air updates on the node (M5).
#   src/  pure C: accepting a command, rebooting when quiet, confirm or roll back,
#         warm restarts, the fw_update entry, the image kind check (M6)
#         (host-tested by firmware/Makefile, test/test_ota.c)
# The ESP-IDF work around these decisions is main/ota_task.c.
idf_component_register(
    SRCS
        "src/image_kind.c"
        "src/ota.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fusion
        ledger
)
