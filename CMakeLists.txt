cmake_minimum_required(VERSION 3.22)

# ── Board selection ──────────────────────────────────────────────────────────
# One firmware tree, two boards. Everything CubeMX generates lives under
# boards/<board>/ (the .ioc, Core/Src, Core/Inc and bsp_config.h); the app and
# BSP layers in Core/ are shared. Pick the board on the command line:
#
#   cmake --preset SPUD-Debug && cmake --build --preset SPUD-Debug
#   cmake --preset UGN-Release && cmake --build --preset UGN-Release
#
# or without presets: cmake -DBOARD=SPUD -DCMAKE_BUILD_TYPE=Debug ...
set(BOARD "UGN" CACHE STRING "Target board: UGN or SPUD")
string(TOUPPER "${BOARD}" BOARD)
if(NOT BOARD MATCHES "^(UGN|SPUD)$")
    message(FATAL_ERROR "BOARD must be UGN or SPUD (got '${BOARD}')")
endif()
string(TOLOWER "${BOARD}" BOARD_DIR)
set(BOARD_PATH ${CMAKE_SOURCE_DIR}/boards/${BOARD_DIR})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_PROJECT_NAME laserfabriken_v2)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
project(${CMAKE_PROJECT_NAME})
message("Board: ${BOARD}   Build type: ${CMAKE_BUILD_TYPE}")

enable_language(C ASM)

add_executable(${CMAKE_PROJECT_NAME})
# Binaries are named after the board so the two can never be mixed up:
# build/<preset>/laserfabriken_spud.elf / .bin
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME laserfabriken_${BOARD_DIR})

# ── CubeMX-generated code for the selected board ─────────────────────────────
file(GLOB BOARD_GENERATED_SRC CONFIGURE_DEPENDS ${BOARD_PATH}/Core/Src/*.c)

# ── Sources ──────────────────────────────────────────────────────────────────
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${BOARD_GENERATED_SRC}
    startup_stm32g431xx.s

    # Shared application layer
    Core/Src/app/pid.c
    Core/Src/app/tec_control.c
    Core/Src/app/laser_control.c
    Core/Src/app/params.c
    Core/Src/app/cli.c
    Core/Src/app/bringup.c
    Core/Src/app/rtt.c
    Core/Src/app/usbc_port.c
    # Shared BSP (pins and constants come from boards/<board>/bsp_config.h)
    Core/Src/bsp/bsp_temp.c
    Core/Src/bsp/bsp_tec.c
    Core/Src/bsp/bsp_laser.c
    Core/Src/bsp/uart_rx.c

    # USB CDC device (hand-modified ST template, shared by both boards)
    USB_Device/Target/usbd_conf.c
    USB_Device/App/usb_device.c
    USB_Device/App/usbd_desc.c
    USB_Device/App/usbd_cdc_if.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c

    # STM32G4 HAL/LL. Union of what both boards use; a module not enabled in
    # the board's stm32g4xx_hal_conf.h compiles to an empty object.
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dac.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dac_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pcd.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pcd_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_spi.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_spi_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usb.c
    Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${BOARD_PATH}            # bsp_config.h
    ${BOARD_PATH}/Core/Inc   # main.h, usart.h, ... (generated)
    Core/Inc/app
    Core/Inc/bsp
    USB_Device/App
    USB_Device/Target
    Drivers/STM32G4xx_HAL_Driver/Inc
    Drivers/STM32G4xx_HAL_Driver/Inc/Legacy
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
    Drivers/CMSIS/Device/ST/STM32G4xx/Include
    Drivers/CMSIS/Include
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    USE_FULL_LL_DRIVER
    USE_HAL_DRIVER
    STM32G431xx
    BOARD_${BOARD}=1
    BOARD_NAME="${BOARD}"
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:CLI_RTT=1>
)

# Work around a toolchain quirk where an implicit library named 'ob' is added.
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

target_link_libraries(${CMAKE_PROJECT_NAME} ${TOOLCHAIN_LINK_LIBRARIES})
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/laserfabriken_${BOARD_DIR}.bin
    COMMENT "Generating laserfabriken_${BOARD_DIR}.bin (${BOARD}, ${CMAKE_BUILD_TYPE})"
)
