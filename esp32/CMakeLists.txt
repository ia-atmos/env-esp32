# The board build of the C implementation (ia-atmos/lang): ESP-IDF v6.
#
#   cd esp32 && idf.py build flash monitor
#   idf.py -DATMOS_APP=<abs path of a .atm> build     another program
#   idf.py -DATMOS_LANG=<checkout of ia-atmos/lang> build
#
# The Lua version is the project at the root of the repository, until
# this one runs on the board.

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(atmos_esp32)
