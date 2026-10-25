# FireX ESP32-CAM bring-up image (AI-Thinker ESP32-CAM, OV2640).
#
#   cd firmware/cam
#   idf.py set-target esp32
#   idf.py build flash
#   python ../../tools/camera/pull_jpeg.py --port COMx
#
# A separate project because the camera is a separate MCU: in M2 the node asks
# it for frames over a UART using the same framed protocol this image speaks to
# the laptop.
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.2.0-m1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firex_cam)
