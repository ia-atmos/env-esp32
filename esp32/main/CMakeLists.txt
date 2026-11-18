# The component of the program: the drivers (drv/esp32/), the runtime of
# ia-atmos/lang (rt/*.c), and the C that the compiler emits for the
# program with the environment env/esp32.atm.

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(ATMOS_LANG "${REPO}/ext/lang" CACHE PATH "checkout of ia-atmos/lang")
set(ATMOS_APP  "${REPO}/app/init.atm" CACHE FILEPATH "the Atmos program")
set(ATMOS_STK  4096 CACHE STRING "entries of the value stack of the runtime")

file(GLOB RT_SRC "${ATMOS_LANG}/rt/*.c")
set(APP_C "${CMAKE_CURRENT_BINARY_DIR}/app.c")

idf_component_register(
    SRCS
        ${RT_SRC}
        "${APP_C}"
        "${REPO}/drv/err.c"
        "${REPO}/drv/esp32/main.c"
        "${REPO}/drv/esp32/clock.c"
        "${REPO}/drv/esp32/mem.c"
        "${REPO}/drv/esp32/gpio.c"
        "${REPO}/drv/esp32/ir.c"
        "${REPO}/drv/esp32/decoder.c"
        "${REPO}/drv/esp32/espnow.c"
        "${REPO}/drv/esp32/prefs.c"
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
        COMMAND "${ATMOS}" -c -e env/esp32.atm "${ATMOS_APP}" > "${APP_C}"
        WORKING_DIRECTORY "${REPO}"
        DEPENDS "${ATMOS}" ${ENV_SRC} ${APP_SRC}
        COMMENT "Compiling ${ATMOS_APP} to C"
        VERBATIM
    )
    set_source_files_properties("${APP_C}" PROPERTIES GENERATED TRUE)

    # the generic driver of the runtime, under another name: app_main
    # (drv/esp32/main.c) calls it
    set_source_files_properties("${ATMOS_LANG}/rt/main.c"
        PROPERTIES COMPILE_DEFINITIONS "main=atmos_main")

    target_compile_definitions(${COMPONENT_LIB} PRIVATE A_STK_MAX=${ATMOS_STK})
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter)
endif()
