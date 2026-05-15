#include <bsp/board.h>
#include <tusb.h>

#include "hardware/gpio.h"

#include "crc.h"
#include "forwarder_control.h"
#include "serial.h"
#include "status_led.h"

#define FORWARDER_UART uart1
#define FORWARDER_TX_PIN 8
#define FORWARDER_RX_PIN 9

bool led_state = false;
bool forwarder_active = false;

bool runtime_report_ok(const uint8_t* buffer, uint16_t bufsize) {
    if (bufsize < RUNTIME_SIZE || ((const runtime_set_feature_t*) buffer)->version != CONFIG_VERSION) {
        return false;
    }

    uint32_t expected_crc = 0;
    memcpy(&expected_crc, buffer + RUNTIME_SIZE - 4, sizeof(expected_crc));
    return crc32(buffer, RUNTIME_SIZE - 4) == expected_crc;
}

void send_forwarder_cursor_report(const runtime_cursor_t& cursor) {
    forwarder_cursor_report_t msg = {
        .report_id = FORWARDER_CONTROL_REPORT_ID,
        .command = FORWARDER_CONTROL_SET_HOST_CURSOR,
        .cursor = cursor,
    };
    serial_write((const uint8_t*) &msg, sizeof(msg), FORWARDER_UART);
}

void send_forwarder_mouse_config_report(const macos_mouse_config_t& mouse_config) {
    forwarder_mouse_config_report_t msg = {
        .report_id = FORWARDER_CONTROL_REPORT_ID,
        .command = FORWARDER_CONTROL_SET_MOUSE_CONFIG,
        .mouse_config = mouse_config,
    };
    serial_write((const uint8_t*) &msg, sizeof(msg), FORWARDER_UART);
}

bool handle_control_packet(const uint8_t* data, uint16_t len) {
    if (len != sizeof(forwarder_control_t) || data[0] != FORWARDER_CONTROL_REPORT_ID) {
        return false;
    }

    const forwarder_control_t* msg = (const forwarder_control_t*) data;
    if (msg->command == FORWARDER_CONTROL_SET_ACTIVE) {
        forwarder_active = msg->value != 0;
        status_led_set_red(forwarder_active);
    }
    return true;
}

void serial_callback(const uint8_t* data, uint16_t len) {
    if (handle_control_packet(data, len)) {
        return;
    }

    if (tud_hid_report(data[0], data + 1, len - 1)) {
        status_led_flash_green();
    }
    board_led_write(led_state);
    led_state = !led_state;
}

void forwarder_serial_init() {
    uart_init(FORWARDER_UART, FORWARDER_BAUDRATE);
    uart_set_translate_crlf(FORWARDER_UART, false);
    gpio_set_function(FORWARDER_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(FORWARDER_RX_PIN, GPIO_FUNC_UART);
}

int main() {
    board_init();
    status_led_init();
    tusb_init();
    forwarder_serial_init();

    while (true) {
        serial_read(serial_callback, FORWARDER_UART);
        tud_task();
        status_led_task();
    }

    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (report_id != REPORT_ID_RUNTIME || !runtime_report_ok(buffer, bufsize)) {
        return;
    }

    const runtime_set_feature_t* runtime_buffer = (const runtime_set_feature_t*) buffer;
    switch (runtime_buffer->command) {
        case RuntimeCommand::SET_HOST_CURSOR: {
            runtime_cursor_t cursor;
            memcpy(&cursor, runtime_buffer->data, sizeof(cursor));
            cursor.active_screen = 1;
            send_forwarder_cursor_report(cursor);
            break;
        }
        case RuntimeCommand::SET_MOUSE_CONFIG: {
            macos_mouse_config_t mouse_config;
            memcpy(&mouse_config, runtime_buffer->data, sizeof(mouse_config));
            send_forwarder_mouse_config_report(mouse_config);
            break;
        }
        default:
            break;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    if (report_id != REPORT_ID_RUNTIME || reqlen < RUNTIME_SIZE) {
        return 0;
    }

    runtime_get_feature_t* runtime_buffer = (runtime_get_feature_t*) buffer;
    memset(runtime_buffer, 0, sizeof(runtime_get_feature_t));

    runtime_status_t* status = (runtime_status_t*) runtime_buffer;
    status->cursor.active_screen = forwarder_active ? 1 : -1;

    runtime_buffer->crc32 = crc32((uint8_t*) runtime_buffer, RUNTIME_SIZE - 4);
    return RUNTIME_SIZE;
}
