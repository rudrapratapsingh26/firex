# FireX node firmware: ESP-IDF project for the ESP32-S3 dev kit.
#
#   idf.py set-target esp32s3
#   idf.py build flash monitor
#
# components/ holds fusion (pure C) and sensors (pure-C scoring plus ESP-IDF
# drivers). test/, sim/ and host-build/ belong to the host build in Makefile and
# are not ESP-IDF components. cam/ is a separate project for the ESP32-CAM.
cmake_minimum_required(VERSION 3.16)

# Bump for every release: a unit refuses an update to the version it already runs.
set(PROJECT_VER "0.6.0-m6")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firex_node)
