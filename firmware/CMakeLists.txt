cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)

set(PICO_COPY_TO_RAM 1)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(OUR_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/src/pio_usb")
set(GENERATED_PIO_HEADERS_PATH "${CMAKE_CURRENT_LIST_DIR}/src/generated_pio")

include(pico_sdk_import.cmake)

project(screenhopper)

pico_sdk_init()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER "10.3.99")
    message(FATAL_ERROR "This firmware needs the original-era ARM GCC 10.3 toolchain. ${CMAKE_C_COMPILER_VERSION} builds UF2s that differ from the checked-in working firmware and can crash with the old Pico SDK/TinyUSB stack. Use firmware/build_original_toolchain.sh, or set PICO_TOOLCHAIN_PATH to an ARM GCC 10.3 install.")
endif()

add_compile_options(-Wall -Wno-array-bounds -Wno-psabi)

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)

add_executable(screenhopper src/remapper.cc src/remapper_single.cc ${OUR_PIO_USB_PATH}/pio_usb.c ${OUR_PIO_USB_PATH}/usb_crc.c src/crc.cc src/descriptor_parser.cc src/pio_usb_stuff.cc src/tinyusb_stuff.cc src/our_descriptor.cc src/macos_pointer_acceleration.cc src/globals.cc src/config.cc src/quirks.cc src/interval_override.cc src/serial.cc src/status_led_noop.cc)

target_include_directories(screenhopper PRIVATE src src/tusb_config_device ${OUR_PIO_USB_PATH} ${GENERATED_PIO_HEADERS_PATH})

target_link_libraries(screenhopper pico_stdlib pico_multicore hardware_pio hardware_dma hardware_flash hardware_clocks tinyusb_device tinyusb_board)

pico_add_extra_outputs(screenhopper)


add_executable(forwarder src/forwarder.cc src/our_descriptor.cc src/tinyusb_stuff.cc src/serial.cc src/crc.cc src/status_led.cc)
target_include_directories(forwarder PRIVATE src src/tusb_config_device ${GENERATED_PIO_HEADERS_PATH})
target_link_libraries(forwarder pico_stdlib hardware_pio hardware_clocks tinyusb_device tinyusb_board)
pico_add_extra_outputs(forwarder)

add_executable(screenhopper_a src/remapper.cc src/remapper_dual_a.cc src/crc.cc src/descriptor_parser.cc src/tinyusb_stuff.cc src/our_descriptor.cc src/macos_pointer_acceleration.cc src/globals.cc src/config.cc src/quirks.cc src/interval_override.cc src/serial.cc src/status_led.cc)
target_include_directories(screenhopper_a PRIVATE src src/tusb_config_device ${GENERATED_PIO_HEADERS_PATH})
target_link_libraries(screenhopper_a pico_stdlib hardware_flash hardware_pio hardware_clocks tinyusb_device tinyusb_board)
pico_add_extra_outputs(screenhopper_a)

add_executable(screenhopper_b src/remapper_dual_b.cc src/crc.cc src/interval_override.cc src/serial.cc src/status_led.cc)
target_include_directories(screenhopper_b PRIVATE src src/tusb_config_host ${GENERATED_PIO_HEADERS_PATH})
target_link_libraries(screenhopper_b pico_stdlib hardware_pio hardware_clocks tinyusb_host tinyusb_board)
pico_add_extra_outputs(screenhopper_b)
