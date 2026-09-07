# ===========================================================================
#  rtos-iot-sensor-node -- FreeRTOS environmental monitoring node
#  Target: STM32F103RBT6 (NUCLEO-F103RB), Cortex-M3, 128 KB flash / 20 KB SRAM
#
#  Configure (CLI):
#     cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
#     cmake --build build
#
#  Configure (CLion): see docs/clion_setup.md
# ===========================================================================
cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(rtos-iot-sensor-node
        VERSION 1.0.0
        LANGUAGES C ASM)

if(NOT CMAKE_C_COMPILER MATCHES "arm-none-eabi")
    message(FATAL_ERROR
        "This project must be configured with the ARM cross toolchain.\n"
        "Add -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake to your CMake options.\n"
        "(To build only the host-side unit tests, configure tests/native instead.)")
endif()

set(TARGET_ELF ${PROJECT_NAME}.elf)

# --------------------------------------------------------------------------
#  Build options
# --------------------------------------------------------------------------
option(USE_UART_DMA      "Use DMA for USART transmit and receive"        ON)
option(SWAP_UART_ROLES   "Put ESP32 telemetry on USART2 and logs on USART1" OFF)

# --------------------------------------------------------------------------
#  MCU / architecture flags
# --------------------------------------------------------------------------
set(MCU_FLAGS
    -mcpu=cortex-m3
    -mthumb
    -mfloat-abi=soft)

add_compile_definitions(
    STM32F103xB
    USE_HAL_DRIVER
    HSE_VALUE=8000000U
    HSI_VALUE=8000000U)

if(USE_UART_DMA)
    add_compile_definitions(APP_USE_UART_DMA=1)
else()
    add_compile_definitions(APP_USE_UART_DMA=0)
endif()

if(SWAP_UART_ROLES)
    add_compile_definitions(APP_SWAP_UART_ROLES=1)
else()
    add_compile_definitions(APP_SWAP_UART_ROLES=0)
endif()

add_compile_options(
    ${MCU_FLAGS}
    -fdata-sections
    -ffunction-sections          # let the linker garbage-collect unused code
    -Wall
    -Wextra
    -Wshadow
    -Wdouble-promotion           # catches accidental float->double on a soft-FP core
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-Og>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-gdwarf-4>
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Release>:-g0>)

add_link_options(
    ${MCU_FLAGS}
    -T${CMAKE_SOURCE_DIR}/linker/STM32F103RB.ld
    --specs=nano.specs           # newlib-nano: small printf, small malloc
    --specs=nosys.specs          # syscalls.c provides the stubs we care about
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map,--cref
    -Wl,--no-warn-rwx-segments)

# --------------------------------------------------------------------------
#  Sources
# --------------------------------------------------------------------------
set(APP_SOURCES
    Core/Src/main.c
    Core/Src/app_tasks.c
    Core/Src/sensor_task.c
    Core/Src/processing_task.c
    Core/Src/comm_task.c
    Core/Src/logging_task.c
    Core/Src/health_task.c
    Core/Src/bme680_driver.c
    Core/Src/uart_dma.c
    Core/Src/telemetry.c
    Core/Src/processing.c
    Core/Src/cmd_parser.c
    Core/Src/perf.c
    Core/Src/stm32f1xx_it.c
    Core/Src/stm32f1xx_hal_msp.c
    Core/Src/freertos_hooks.c
    Core/Src/syscalls.c)

set(HAL_SOURCES
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_i2c.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c
    Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c)

set(FREERTOS_SOURCES
    Middleware/FreeRTOS-Kernel/tasks.c
    Middleware/FreeRTOS-Kernel/queue.c
    Middleware/FreeRTOS-Kernel/list.c
    Middleware/FreeRTOS-Kernel/timers.c
    Middleware/FreeRTOS-Kernel/event_groups.c
    Middleware/FreeRTOS-Kernel/stream_buffer.c
    Middleware/FreeRTOS-Kernel/portable/GCC/ARM_CM3/port.c
    Middleware/FreeRTOS-Kernel/portable/MemMang/heap_4.c)

set(VENDOR_SOURCES
    Drivers/BME68x_SensorAPI/bme68x.c
    Drivers/CMSIS/Device/ST/STM32F1xx/Source/system_stm32f1xx.c
    Drivers/CMSIS/Device/ST/STM32F1xx/Source/startup_stm32f103xb.s)

add_executable(${TARGET_ELF}
    ${APP_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
    ${VENDOR_SOURCES})

target_include_directories(${TARGET_ELF} PRIVATE
    Core/Inc
    Drivers/STM32F1xx_HAL_Driver/Inc
    Drivers/STM32F1xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32F1xx/Include
    Drivers/CMSIS/Core/Include
    Drivers/BME68x_SensorAPI
    Middleware/FreeRTOS-Kernel/include
    Middleware/FreeRTOS-Kernel/portable/GCC/ARM_CM3)

# Vendor code is not ours to keep -Wall-clean; silence it without hiding our own
# warnings. (Bosch's driver triggers -Wshadow, ST's HAL triggers -Wunused.)
set_source_files_properties(${HAL_SOURCES} ${VENDOR_SOURCES} ${FREERTOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w")

# --------------------------------------------------------------------------
#  Post-build: .hex / .bin + size report
# --------------------------------------------------------------------------
add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET_ELF}> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET_ELF}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE_UTIL} --format=berkeley $<TARGET_FILE:${TARGET_ELF}>
    COMMENT "Generating .hex / .bin and reporting image size")

# --------------------------------------------------------------------------
#  Convenience targets: flash / erase / debug-server  (need OpenOCD on PATH)
# --------------------------------------------------------------------------
set(OPENOCD_CFG ${CMAKE_SOURCE_DIR}/openocd/stm32f103rb.cfg)

add_custom_target(flash
    COMMAND openocd -f ${OPENOCD_CFG}
            -c "program $<TARGET_FILE:${TARGET_ELF}> verify reset exit"
    DEPENDS ${TARGET_ELF}
    COMMENT "Flashing ${TARGET_ELF} through ST-LINK")

add_custom_target(erase
    COMMAND openocd -f ${OPENOCD_CFG} -c "init; reset halt; stm32f1x mass_erase 0; exit"
    COMMENT "Mass-erasing internal flash")

add_custom_target(gdbserver
    COMMAND openocd -f ${OPENOCD_CFG}
    COMMENT "Starting OpenOCD GDB server on :3333")
