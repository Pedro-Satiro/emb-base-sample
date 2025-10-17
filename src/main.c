#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(sensors, LOG_LEVEL_INF);

#define TEMP_MIN            
#define TEMP_MAX            30
#define HUMIDITY_MIN        40
#define HUMIDITY_MAX        70

#define STACK_SIZE          1024
#define PRIORITY            5
#define QUEUE_SIZE          10

#define TEMP_INTERVAL_MS    800
#define HUMIDITY_INTERVAL_MS 600

typedef enum {
    SENSOR_HUMIDITY = 0,
    SENSOR_TEMPERATURE
} sensor_type_t;

typedef struct {
    sensor_type_t type;
    union {
        uint8_t humidity;
        int8_t temperature;
    } value;
    int64_t timestamp;
} sensor_msg_t;

K_MSGQ_DEFINE(input_queue, sizeof(sensor_msg_t), QUEUE_SIZE, 4);
K_MSGQ_DEFINE(output_queue, sizeof(sensor_msg_t), QUEUE_SIZE, 4);

static sensor_msg_t generate_sensor_reading(sensor_type_t type) {
    sensor_msg_t msg;
    msg.type = type;
    msg.timestamp = k_uptime_get();
    
    if (type == SENSOR_HUMIDITY) {
        msg.value.humidity = sys_rand32_get() % 100;
    } else {
        msg.value.temperature = (sys_rand32_get() % 50) + 10;
    }
    
    return msg;
}

static void temperature_producer_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Temperature Producer: Started");
    
    while (1) {
        sensor_msg_t msg = generate_sensor_reading(SENSOR_TEMPERATURE);
        
        int ret = k_msgq_put(&input_queue, &msg, K_NO_WAIT);
        if (ret != 0) {
            LOG_WRN("Temperature Producer: Queue full, dropping reading");
        }
        
        k_msleep(TEMP_INTERVAL_MS);
    }
}

static void humidity_producer_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Humidity Producer: Started");
    
    while (1) {
        sensor_msg_t msg = generate_sensor_reading(SENSOR_HUMIDITY);
        
        int ret = k_msgq_put(&input_queue, &msg, K_NO_WAIT);
        if (ret != 0) {
            LOG_WRN("Humidity Producer: Queue full, dropping reading");
        }
        
        k_msleep(HUMIDITY_INTERVAL_MS);
    }
}
static bool validate_sensor_data(const sensor_msg_t *msg) {
    switch (msg->type) {
        case SENSOR_TEMPERATURE:
            return (msg->value.temperature >= TEMP_MIN && 
                    msg->value.temperature <= TEMP_MAX);
        case SENSOR_HUMIDITY:
            return (msg->value.humidity >= HUMIDITY_MIN && 
                    msg->value.humidity <= HUMIDITY_MAX);
        default:
            return false;
    }
}

static void log_invalid_data(const sensor_msg_t *msg) {
    if (msg->type == SENSOR_TEMPERATURE) {
        LOG_ERR("INVALID Temperature: %d°C (valid range: %d-%d°C)", 
                msg->value.temperature, TEMP_MIN, TEMP_MAX);
    } else {
        LOG_ERR("INVALID Humidity: %d%% (valid range: %d-%d%%)", 
                msg->value.humidity, HUMIDITY_MIN, HUMIDITY_MAX);
    }
}

static void filter_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Filter: Started");
    sensor_msg_t msg;
    
    while (1) {
        if (k_msgq_get(&input_queue, &msg, K_FOREVER) == 0) {
            if (validate_sensor_data(&msg)) {
                int ret = k_msgq_put(&output_queue, &msg, K_NO_WAIT);
                if (ret != 0) {
                    LOG_WRN("Filter: Output queue full, dropping valid data");
                }
            } else {
                log_invalid_data(&msg);
            }
        }
    }
}

static void consumer_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Consumer: Started");
    sensor_msg_t msg;
    
    while (1) {
        if (k_msgq_get(&output_queue, &msg, K_FOREVER) == 0) {
            if (msg.type == SENSOR_TEMPERATURE) {
                LOG_INF("✓ VALID Temperature: %d°C [ts: %lld]", 
                        msg.value.temperature, msg.timestamp);
            } else {
                LOG_INF("✓ VALID Humidity: %d%% [ts: %lld]", 
                        msg.value.humidity, msg.timestamp);
            }
        }
    }
}

K_THREAD_DEFINE(temp_producer, STACK_SIZE, temperature_producer_thread, 
                NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(humidity_producer, STACK_SIZE, humidity_producer_thread, 
                NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(filter, STACK_SIZE, filter_thread, 
                NULL, NULL, NULL, PRIORITY + 1, 0, 0);

K_THREAD_DEFINE(consumer, STACK_SIZE, consumer_thread, 
                NULL, NULL, NULL, PRIORITY + 2, 0, 0);

int main(void) {
    LOG_INF("=== Sensor Data Processing System ===");
    LOG_INF("Temperature range: %d-%d°C", TEMP_MIN, TEMP_MAX);
    LOG_INF("Humidity range: %d-%d%%", HUMIDITY_MIN, HUMIDITY_MAX);
    LOG_INF("==========================================");
    
    while (1) {
        k_msleep(5000);
        LOG_INF("System running - Queues: Input[%d] Output[%d]", 
                k_msgq_num_used_get(&input_queue),
                k_msgq_num_used_get(&output_queue));
    }
    
    return 0;
}