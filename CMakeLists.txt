cmake_minimum_required(VERSION 3.16)

# Must be set before project.cmake is included.
set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-p4-motion-cam)
