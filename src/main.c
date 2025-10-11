#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device* const console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct gpio_callback button_cb_data;
#define STEPS       50U
#define PWM_PERIOD  20000U
#define BLINK_MS    500U
#define FADE_MS     25U
#define FADE_STEP   (PWM_PERIOD / STEPS)
#define LOG_MS      100U 

enum operation_mode {
    MODE_BLINK = 0,
    MODE_PWM
};

static struct {
    volatile enum operation_mode current_mode;
    uint32_t pulse_width_us;
    uint8_t fade_direction;
    int64_t last_log_time;
    bool led_is_on;
} state = {
    .current_mode = MODE_BLINK,
    .fade_direction = 1U,
    .led_is_on = false,
};

static void run_blink(void) {
    gpio_pin_toggle_dt(&led0);
    state.led_is_on = !state.led_is_on;
    
    int64_t now = k_uptime_get();
    if (now - state.last_log_time >= LOG_MS) {
        LOG_INF("Blink Mode - LED: %s", state.led_is_on ? "ON" : "OFF");
        state.last_log_time = now;
    }

    k_msleep(BLINK_MS);
}

static inline void sw_pwm_cycle(void) {
    gpio_pin_set_dt(&led0, 1);
    if (state.pulse_width_us > 0) {
        k_busy_wait(state.pulse_width_us);
    }
    
    gpio_pin_set_dt(&led0, 0);
    uint32_t off_time = PWM_PERIOD - state.pulse_width_us;
    if (off_time > 0) {
        k_busy_wait(off_time);
    }
}

static void run_pwm(void) {
    sw_pwm_cycle();
    
    // Log throttling
    int64_t now = k_uptime_get();
    if (now - state.last_log_time >= LOG_MS) {
        LOG_INF("Fade Mode: Brightness at ~%d%%", 
            (100 * state.pulse_width_us) / PWM_PERIOD);
        state.last_log_time = now;
    }
    
    // Optimized fade logic
    if (state.fade_direction) {
        state.pulse_width_us += FADE_STEP;
        if (state.pulse_width_us >= PWM_PERIOD) {
            state.pulse_width_us = PWM_PERIOD;
            state.fade_direction = 0U;
        }
    } else {
        if (state.pulse_width_us <= FADE_STEP) {
            state.pulse_width_us = 0;
            state.fade_direction = 1U;
        } else {
            state.pulse_width_us -= FADE_STEP;
        }
    }

    k_msleep(FADE_MS);
}

static void toggle_mode(void) {
    if (state.current_mode == MODE_BLINK) {
        state.current_mode = MODE_PWM;
        state.pulse_width_us = 0;
        state.fade_direction = 1U;
        LOG_INF("-------------------------------------");
        LOG_INF("Mode changed to: PWM");
        LOG_INF("----------------------------------");
    } else {
        state.current_mode = MODE_BLINK;
        LOG_INF("-------------------------------------");
        LOG_INF("Switching to BLINK mode in 3 seconds...");
        LOG_INF("-------------------------------------");
        k_msleep(3000);
        // Resolve double click issue
        gpio_pin_set_dt(&led0, 0);
        LOG_INF("-------------------------------------");
        LOG_INF("Mode changed to: BLINK");
        LOG_INF("----------------------------------");
    }
}

void button_pressed(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    
    toggle_mode();
}

int main(void) {
    unsigned char c;
    
    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    LOG_INF("=== LED Control Application ===");
    LOG_INF("Modes: BLINK <-> PWM (Fade)");
    LOG_INF("Current mode: %s", state.current_mode == MODE_BLINK ? "BLINK" : "PWM");
    LOG_INF("===============================================");
    
    while (1) {
        if (!uart_poll_in(console_dev, &c) && (c == '\n' || c == '\r')) {
            toggle_mode();
        }
        switch (state.current_mode) {
            case MODE_PWM:
                run_pwm();
                break;
            case MODE_BLINK:
                run_blink();
                break;
        }
    }
    return 0;
}