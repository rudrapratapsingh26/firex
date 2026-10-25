# The FireX node application (M3): alarm, fusion, ledger, camera trigger,
# self-test, the M1 capture stream, the console, and the link to the site gateway.
idf_component_register(
    SRCS
        "alarm_task.c"
        "camera_task.c"
        "console.c"
        "console_security.c"
        "fusion_task.c"
        "image_kind.c"
        "ledger_task.c"
        "net_task.c"
        "node_main.c"
        "ota_task.c"
        "reporter.c"
        "selftest_task.c"
        "settings_nvs.c"
        "trace_task.c"
    INCLUDE_DIRS "."
    REQUIRES
        alarm
        core
        fusion
        ledger
        media
        net
        ota
        secure
        sensors
        trace
        app_update
        console
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_http_client
        esp_partition
        esp_timer
        mqtt
        nvs_flash
        vfs
)

# The image kind is read from the image, never referenced by code that could drop it (M6).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fx_image_kind")

# Timestamps start from the build time on a fresh log (see fx_clock_init).
string(TIMESTAMP FX_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FX_BUILD_EPOCH=${FX_BUILD_EPOCH}ULL)
