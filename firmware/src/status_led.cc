#include "status_led.h"

#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "status_ws2812.pio.h"

const uint STATUS_LED_PIN = 23;
const uint32_t WS2812_FREQ = 800000;
const uint64_t GREEN_FLASH_US = 100000;
const uint8_t RED_LEVEL = 16;
const uint8_t GREEN_LEVEL = 16;

PIO status_pio;
int status_sm = -1;
bool status_initialized = false;
bool red_on = false;
bool green_on = false;
bool dirty = true;
uint64_t green_until = 0;

bool init_status_pio(PIO pio) {
    if (!pio_can_add_program(pio, &status_ws2812_program)) {
        return false;
    }

    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        return false;
    }

    uint offset = pio_add_program(pio, &status_ws2812_program);
    status_ws2812_program_init(pio, (uint) sm, offset, STATUS_LED_PIN, (float) WS2812_FREQ);

    status_pio = pio;
    status_sm = sm;
    return true;
}

bool write_status_led() {
    if (!status_initialized || status_sm < 0) {
        return true;
    }

    uint8_t green = green_on ? GREEN_LEVEL : 0;
    uint8_t red = red_on ? RED_LEVEL : 0;
    uint32_t pixel_grb = ((uint32_t) green << 24) | ((uint32_t) red << 16);

    if (pio_sm_is_tx_fifo_full(status_pio, (uint) status_sm)) {
        return false;
    }

    pio_sm_put(status_pio, (uint) status_sm, pixel_grb);
    return true;
}

void status_led_init() {
    if (status_initialized) {
        return;
    }

    status_initialized = init_status_pio(pio0) || init_status_pio(pio1);
    dirty = true;
    status_led_task();
}

void status_led_set_red(bool on) {
    if (red_on != on) {
        red_on = on;
        dirty = true;
    }
}

void status_led_flash_green() {
    green_until = time_us_64() + GREEN_FLASH_US;
    if (!green_on) {
        green_on = true;
        dirty = true;
    }
}

void status_led_task() {
    if (green_on && (int64_t) (time_us_64() - green_until) >= 0) {
        green_on = false;
        dirty = true;
    }

    if (dirty && write_status_led()) {
        dirty = false;
    }
}
