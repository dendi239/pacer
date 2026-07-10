cmake_minimum_required(VERSION 3.16)

# In-kart live timing dashboard: ESP32-S3 + 25 Hz u-blox GPS + SPI TFT.
# Reuses the desktop pacer C++ core (see components/pacer_core).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pacer_dashboard)
