# ===========================================================================
#  Host build of the platform-independent firmware logic.
#
#  processing.c, telemetry.c and cmd_parser.c contain no HAL or FreeRTOS calls,
#  so they compile natively and can be exercised from Python through ctypes.
#  This is what makes tests/test_processing.py and tests/test_uart_protocol.py
#  real unit tests of the shipped firmware code rather than a re-implementation.
#
#     cmake -S tests/native -B tests/native/build
#     cmake --build tests/native/build
#     pytest tests -q
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(app_logic_host LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

add_library(app_logic SHARED
    ${REPO_ROOT}/Core/Src/processing.c
    ${REPO_ROOT}/Core/Src/telemetry.c
    ${REPO_ROOT}/Core/Src/cmd_parser.c)

target_include_directories(app_logic PUBLIC ${REPO_ROOT}/Core/Inc)

# app_config.h keys the UART role assignment off these; the values are
# irrelevant to the logic under test but the macros must exist.
target_compile_definitions(app_logic PRIVATE
    APP_SWAP_UART_ROLES=0
    APP_USE_UART_DMA=0
    HOST_BUILD=1)

target_compile_options(app_logic PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion -Wno-unused-parameter -O1 -g)
