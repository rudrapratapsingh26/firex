# Camera link on the node.
#   src/  pure C: reply parser that hashes JPEG bytes as they arrive (host-tested)
#   hw/   UART transport to the ESP32-CAM
idf_component_register(
    SRCS
        "src/frame_rx.c"
        "hw/cam_link.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ledger
        esp_driver_uart
        esp_timer
)
