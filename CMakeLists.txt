cmake_minimum_required(VERSION 3.16)

# CRITICAL: Must be BEFORE include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cam_lcd)