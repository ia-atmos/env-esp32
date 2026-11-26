# The component of the program: the drivers (drv/esp32/), the runtime of
# ia-atmos/lang (rt/*.c), and the C that the compiler emits for the
# program, which includes the modules of env/ that it uses.

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(ATMOS_LANG "${REPO}/ext/lang" CACHE PATH "checkout of ia-atmos/lang")
set(ATMOS_APP  "${REPO}/app/init.atm" CACHE FILEPATH "the Atmos program")
# 16 bytes each: app/init.atm needs fewer than 64 (measured on the host)
set(ATMOS_STK  1024 CACHE STRING "entries of the value stack of the runtime")
# reals as C float, in the single-precision FPU of the ESP32 (atmos-lang#3)
option(ATMOS_REAL_FLOAT "reals are 32-bit floats" OFF)
# integers of 32 bits (with ATMOS_REAL_FLOAT, a value has 8 bytes instead
# of 16), and the time in milliseconds (with 32-bit integers, the time
# since boot in microseconds would wrap after 35 minutes): use both
option(ATMOS_INT_32  "integers are 32 bits" OFF)
option(ATMOS_TIME_MS "the unit of time is the millisecond" OFF)
# the clock never blocks and each step emits :fps (env/debug.atm counts
# them): the most steps per second, to evaluate the implementation
option(ESP32_FPS "steps as fast as possible, each emitting :fps" OFF)
set(ATMOS_PIN "" CACHE STRING "checks of pin: empty (all), unchecked, or off (pin for tasks and pools only, no collection of cycles)")

file(GLOB RT_SRC "${ATMOS_LANG}/rt/*.c")
set(APP_C "${CMAKE_CURRENT_BINARY_DIR}/app.c")

idf_component_register(
    SRCS
        ${RT_SRC}
        "${APP_C}"
        "${REPO}/drv/err.c"
        "${REPO}/drv/src.c"
        "${REPO}/drv/esp32/main.c"
        "${REPO}/drv/esp32/clock.c"
        "${REPO}/drv/esp32/mem.c"
        "${REPO}/drv/esp32/gpio.c"
        "${REPO}/drv/esp32/ir.c"
        "${REPO}/drv/esp32/decoder.c"
        "${REPO}/drv/esp32/espnow.c"
        "${REPO}/drv/esp32/wifi.c"
        "${REPO}/drv/esp32/prefs.c"
        "${REPO}/drv/esp32/sleep.c"
    INCLUDE_DIRS
        "${REPO}/drv"
        "${ATMOS_LANG}/rt"
    REQUIRES
        esp_timer esp_rom esp_driver_gpio esp_driver_ledc esp_driver_rmt
        esp_adc nvs_flash esp_wifi esp_event esp_netif
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # the compiler, a host tool, built with the host C compiler
    set(ATMOS "${ATMOS_LANG}/atmos")
    file(GLOB ATMOS_CC_SRC "${ATMOS_LANG}/src/*.c" "${ATMOS_LANG}/src/*.h")
    add_custom_command(
        OUTPUT "${ATMOS}"
        COMMAND ${CMAKE_COMMAND} -E env "PATH=/usr/bin:/bin:$ENV{PATH}"
                make -C "${ATMOS_LANG}" atmos CC=cc
        DEPENDS ${ATMOS_CC_SRC}
        COMMENT "Building the Atmos compiler (host)"
    )

    # the program, with the environment, as C
    file(GLOB ENV_SRC "${REPO}/env/*.atm")
    get_filename_component(APP_DIR "${ATMOS_APP}" DIRECTORY)
    file(GLOB_RECURSE APP_SRC "${APP_DIR}/*.atm")
    add_custom_command(
        OUTPUT "${APP_C}"
        COMMAND "${ATMOS}" -c "${ATMOS_APP}" > "${APP_C}"
        WORKING_DIRECTORY "${REPO}"
        DEPENDS "${ATMOS}" ${ENV_SRC} ${APP_SRC}
        COMMENT "Compiling ${ATMOS_APP} to C"
        VERBATIM
    )
    set_source_files_properties("${APP_C}" PROPERTIES GENERATED TRUE)

    # the generic driver of the runtime, under another name: app_main
    # (drv/esp32/main.c) calls it; with no clock of its own (no
    # nanosleep in newlib of ESP-IDF), since env/clock.atm is the clock
    set_source_files_properties("${ATMOS_LANG}/rt/main.c"
        PROPERTIES COMPILE_DEFINITIONS "main=atmos_main;ATM_NO_CLOCK")

    target_compile_definitions(${COMPONENT_LIB} PRIVATE A_STK_MAX=${ATMOS_STK})
    if(ATMOS_REAL_FLOAT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ATM_FLOAT_32)
    endif()
    if(ATMOS_INT_32)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ATM_INTEGER_32)
    endif()
    if(ATMOS_PIN STREQUAL "unchecked")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ATM_PIN_UNCHECKED)
    elseif(ATMOS_PIN STREQUAL "off")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ATM_PIN_OFF)
    elseif(NOT ATMOS_PIN STREQUAL "")
        message(FATAL_ERROR "ATMOS_PIN: expected unchecked or off, got '${ATMOS_PIN}'")
    endif()
    if(ESP32_FPS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32_FPS)
    endif()
    if(ATMOS_TIME_MS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ATM_CLOCK_MS)
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter)
endif()
