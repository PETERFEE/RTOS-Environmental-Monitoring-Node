# ---------------------------------------------------------------------------
# CMake toolchain file for the GNU Arm Embedded toolchain (arm-none-eabi-gcc).
#
# CLion: Settings -> Build, Execution, Deployment -> CMake -> Profile ->
#        "CMake options":  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
# CLI:   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Building a static library instead of a test executable stops CMake from
# trying to link (and run) a hosted test program during compiler detection.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TOOLCHAIN_PREFIX arm-none-eabi-)

# Allow an explicit toolchain location, e.g. -DTOOLCHAIN_DIR=/opt/gcc-arm/bin
if(DEFINED TOOLCHAIN_DIR)
    set(TOOLCHAIN_PREFIX "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}")
endif()

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy CACHE FILEPATH "objcopy")
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}objdump CACHE FILEPATH "objdump")
set(CMAKE_SIZE_UTIL    ${TOOLCHAIN_PREFIX}size    CACHE FILEPATH "size")

# Only look for programs on the host; headers/libs come from the toolchain.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
