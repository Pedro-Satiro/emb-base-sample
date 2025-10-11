#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device* const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct gpio_callback btn_cb;

#define STEPS           50U
#define PWM_T           20000U
#define BLINK_T         500U
#define FADE_T          25U
#define STEP_US         (PWM_T / STEPS)
#define LOG_T           100U 

typedef enum {
    BLINK = 0,
    FADE
} mode_t;

static struct ctrl {
    volatile mode_t mode;
    uint32_t duty;
    uint8_t dir;
    int64_t ts;
    bool on;
} st = {
    .mode = BLINK,
    .dir = 1U,
    .on = false,
    .duty = 0,
    .ts = 0
};

static void run_blink(void) {
    gpio_pin_toggle_dt(&led);
    st.on = !st.on;
    
    int64_t now = k_uptime_get();
    if (now - st.ts >= LOG_T) {
        LOG_INF("Blink Mode - LED: %s", st.on ? "ON" : "OFF");
        st.ts = now;
    }

    k_msleep(BLINK_T);
}

static inline void pwm_gen(void) {
    gpio_pin_set_dt(&led, 1);
    if (st.duty > 0) {
        k_busy_wait(st.duty);
    }
    
    gpio_pin_set_dt(&led, 0);
    uint32_t off_us = PWM_T - st.duty;
    if (off_us > 0) {
        k_busy_wait(off_us);
    }
}

static void run_fade(void) {
    pwm_gen();
    
    int64_t now = k_uptime_get();
    if (now - st.ts >= LOG_T) {
        LOG_INF("Fade Mode: Brightness at ~%d%%", 
            (100 * st.duty) / PWM_T);
        st.ts = now;
    }
    
    if (st.dir) {
        st.duty += STEP_US;
        if (st.duty >= PWM_T) {
            st.duty = PWM_T;
            st.dir = 0U;
        }
    } else {
        if (st.duty <= STEP_US) {
            st.duty = 0;
            st.dir = 1U;
        } else {
            st.duty -= STEP_US;
        }
    }

    k_msleep(FADE_T);
}

static void toggle(void) {
    if (st.mode == BLINK) {
        st.mode = FADE;
        st.duty = 0;
        st.dir = 1U;
        LOG_INF("-------------------------------------");
        LOG_INF("Mode changed to: FADE");
        LOG_INF("-------------------------------------");
    } else {
        st.mode = BLINK;
        LOG_INF("-------------------------------------");
        LOG_INF("Switching to BLINK mode in 3 seconds...");
        LOG_INF("-------------------------------------");
        k_msleep(3000);
        gpio_pin_set_dt(&led, 0);
        LOG_INF("-------------------------------------");
        LOG_INF("Mode changed to: BLINK");
        LOG_INF("-------------------------------------");
    }
}

void btn_isr(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    
    toggle();
}

int main(void) {
    unsigned char c;
    
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&btn, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&btn_cb, btn_isr, BIT(btn.pin));
    gpio_add_callback(btn.port, &btn_cb);

    LOG_INF("=== LED Control Application ===");
    LOG_INF("Modes: BLINK <-> FADE (PWM)");
    LOG_INF("Current mode: %s", st.mode == BLINK ? "BLINK" : "FADE");
    LOG_INF("Press ENTER or button to switch modes");
    LOG_INF("===============================================");
    
    while (1) {
        if (!uart_poll_in(uart, &c) && 
            (c == '\n' || c == '\r')) {
            toggle();
        }
        
        switch (st.mode) {
            case FADE:
                run_fade();
                break;
            case BLINK:
                run_blink();
                break;
        }
    }
    return 0;
}

// west build -b mps2/an385 -d build
// timeout 20s west build -t run