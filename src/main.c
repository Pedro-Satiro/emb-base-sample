
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
// https://docs.zephyrproject.org/latest/genindex.html


LOG_MODULE_REGISTER(main);


#define HTIMER CONFIG_HELLO_TIMER

static void hello_timer_callback(struct k_timer *timer)
{
        LOG_WRN("--2--  Warning teste");
        LOG_INF("--3--Hello World!");   
        LOG_ERR("--1--Erro teste");     
        LOG_DBG("--4--Debug info"); 
        printf("------------------- \n");
}
// k_timer_start(&hello_timer, K_MSEC(2000), K_MSEC(2000));
K_TIMER_DEFINE(hello_timer, hello_timer_callback, NULL);            

int main(void)
{
    LOG_INF("--------Inicio--------\n");

    // printf("--------Inicio--------\n");

    k_timer_start(&hello_timer, K_MSEC(HTIMER), K_MSEC(HTIMER));
    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}

// west build -p -b qemu_cortex_m3
// west build -t run