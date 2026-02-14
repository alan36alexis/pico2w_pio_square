cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Importar Raspberry Pi Pico SDK (debe ser antes de project())
include(pico_sdk_import.cmake)

project(pico2w_pio_square C CXX ASM)

# Inicializar Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(${PROJECT_NAME})

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/stepgen.pio)

target_sources(${PROJECT_NAME} PRIVATE stepgen.c)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pico_stdlib
    hardware_dma
    hardware_irq
    hardware_pio
    m
)

# Habilitar stdio por UART y deshabilitar USB
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_usb(${PROJECT_NAME} 0)

# Pin de salida STEP: cmake -DSTEPGEN_PIN=<gpio>
if(STEPGEN_PIN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        STEPGEN_PIN=${STEPGEN_PIN}
    )
endif()

pico_add_extra_outputs(${PROJECT_NAME})
