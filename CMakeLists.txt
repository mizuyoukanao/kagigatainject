# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# KagigataDisk: bare RP2354A (RP2350A package, 2 MiB on-die flash).
set(PICO_BOARD kagigatadisk CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Target platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD_HEADER_DIRS ${PICO_BOARD_HEADER_DIRS} CACHE STRING "" FORCE)

include(pico_sdk_import.cmake)

project(kagigatainject C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(kagigatainject
    src/main.c
    src/sd_responder.c
    src/sd_proto.c
    src/sdlog.c
    src/config.c
)

pico_generate_pio_header(kagigatainject ${CMAKE_CURRENT_LIST_DIR}/src/sd_cmd.pio)

# core1 runs these from RAM while core0 may be writing flash, so GCC must not
# turn their zero-fill/copy loops into memset/memcpy calls: depending on the
# SDK and toolchain those live in flash. tools/check_core1_ram.py verifies.
set_source_files_properties(src/sd_proto.c src/sd_responder.c PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

target_include_directories(kagigatainject PRIVATE src)

set(KG_FW_VERSION "1.0.0" CACHE STRING "firmware version string")
target_compile_definitions(kagigatainject PRIVATE
    KG_FW_VERSION="${KG_FW_VERSION}"
    # main() starts the SD responder before setting up the crystal and PLLs,
    # then calls these two itself (see main.c).
    PICO_RUNTIME_SKIP_INIT_CLOCKS=1
    PICO_RUNTIME_SKIP_INIT_POST_CLOCK_RESETS=1
    # USB CDC console; 1200 baud still resets into BOOTSEL.
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
)
target_compile_options(kagigatainject PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(kagigatainject
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_ticks
)

pico_enable_stdio_usb(kagigatainject 1)
pico_enable_stdio_uart(kagigatainject 0)

pico_add_extra_outputs(kagigatainject)

# Host-side unit test of the protocol logic (no SDK needed):
#   gcc -DSD_PROTO_HOST_TEST -Isrc test/test_sd_proto.c src/sd_proto.c
