# The fusion engine as an ESP-IDF component. The same two files build on the
# host through firmware/Makefile; nothing here may include a hardware header.
idf_component_register(
    SRCS "fusion.c"
    INCLUDE_DIRS "include"
)
