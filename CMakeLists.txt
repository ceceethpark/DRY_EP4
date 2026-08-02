cmake_minimum_required(VERSION 3.16)

set(CMAKE_OBJECT_PATH_MAX 512)
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/inc")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(dy_ep4 VERSION 1.0.0)
