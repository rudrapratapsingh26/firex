# Node logic with no hardware: ledger payloads, settings schema, self-test
# judgements, crossings and time. Host-tested by firmware/Makefile.
idf_component_register(
    SRCS
        "src/events.c"
        "src/selftest.c"
        "src/settings.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fusion
        sensors
        ledger
)

# Production images fix the camera's frames per CRITICAL in firmware (fx_settings_locked).
if(CONFIG_FIREX_PRODUCTION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FX_CAM_FRAMES_LOCKED=${CONFIG_FIREX_CAM_FRAMES_LOCKED})
endif()
