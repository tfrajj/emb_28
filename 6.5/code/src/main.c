#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "CONVEYOR";

// Pin definitions.
#define PIN_BTN_COUNT GPIO_NUM_15 // Button 1: Optical sensor immitation.
#define PIN_BTN_STOP  GPIO_NUM_16 // Button 2: Emergency stop button.
#define PIN_LED_ALARM GPIO_NUM_10 // LED: Alarm indicator.

// LDR as random number generator.
#define LDR_ADC_UNIT ADC_UNIT_1
#define LDR_ADC_CHANNEL ADC_CHANNEL_3

// Debounce delay for button presses.
#define DEBOUNCE_DELAY_US 50000

// Global part counter variable (Updated by ISR, read by task 1).
volatile uint32_t g_part_count = 0;

// Spinlock for synchronizing access to g_part_count and for core stop flag.
static portMUX_TYPE g_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Conveyor emergency stop flag (Updated by ISR, read by task 1).
volatile bool g_system_halted = false;

// System report shared buffer (General for task 1, task 2, task 3).
char g_report_buffer[256] = "System initializing...";

// Mutex to protect g_report_buffer access.
SemaphoreHandle_t g_report_mutex = NULL;

// Binary semaphore to wake up task 3 when emergency stop is triggered.
SemaphoreHandle_t g_emergency_semaphore = NULL;

// ADC descriptor.
adc_oneshot_unit_handle_t g_adc_handle = NULL;


// ISR for parts counting (Button 1).
static void IRAM_ATTR btn_count_isr_handler(void *arg) {
    static int64_t last_press_time = 0;
    int64_t now = esp_timer_get_time();

    // Debounce.
    if ((now - last_press_time) > DEBOUNCE_DELAY_US) {
        last_press_time = now;

        // Increment part count.
        portENTER_CRITICAL_ISR(&g_spinlock);
        if (!g_system_halted) {
            g_part_count++;
        }
        portEXIT_CRITICAL_ISR(&g_spinlock);
    }
}

// ISR for emergency stop (Button 2).
static void IRAM_ATTR btn_stop_isr_handler(void *arg) {
   BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Free up the semaphore to wake up task 3 for emergency stop handling.
    xSemaphoreGiveFromISR(g_emergency_semaphore, &xHigherPriorityTaskWoken);

    // Switch context to task 3 if it has a higher priority than the current task.
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Task 1: Conveyor speed monitoring and reporting.
void task_conveyor_speed(void *pvParameters) {
    uint32_t prev_count = 0;

    while (1) {
        // Simulate conveyor speed control logic.
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Check if the system is halted.
        if (g_system_halted) {
            continue;
        }

        // Safely read the part count.
        uint32_t current_count;
        portENTER_CRITICAL(&g_spinlock);
        current_count = g_part_count;
        portEXIT_CRITICAL(&g_spinlock);

        // Parts amount processed in the last second.
        uint32_t delta = current_count - prev_count;
        uint32_t speed_per_minute = delta * 60; // Convert to parts per minute.
        prev_count = current_count;

        if (xSemaphoreTake(g_report_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!g_system_halted) {
                snprintf(
                    g_report_buffer,
                    sizeof(g_report_buffer),
                    "[Core %d] Parts: %lu | Speed: %lu parts/min",
                    xPortGetCoreID(),
                    (unsigned long) current_count,
                    (unsigned long) speed_per_minute
                );
            }

            xSemaphoreGive(g_report_mutex);
        }
    }
}

// Task 2: ADC sensor reading and reporting.
void task_adc_sensor(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(500)); // Initial delay to allow system stabilization.

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (g_system_halted) {
            continue;
        }

        int adc_raw = 0;
        adc_oneshot_read(g_adc_handle, LDR_ADC_CHANNEL, &adc_raw);

        if (xSemaphoreTake(g_report_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!g_system_halted) {
                size_t len = strlen(g_report_buffer);
                snprintf(
                    g_report_buffer + len,
                    sizeof(g_report_buffer) - len,
                    " || [Core %d] ADC Raw: %d",
                    xPortGetCoreID(),
                    adc_raw
                );

                // Log the updated report buffer.
                ESP_LOGI(TAG, "%s", g_report_buffer);
            }

            xSemaphoreGive(g_report_mutex);
        }
    }
}

void task_emergency_halt(void *pvParameters) {
    while (1) {
        // Sleep (0%  CPU) until the emergency semaphore is given by the ISR.
        if (xSemaphoreTake(g_emergency_semaphore, portMAX_DELAY) == pdTRUE) {
            gpio_set_level(PIN_LED_ALARM, 1); // Turn on the alarm LED.

            // Set the system halted flag.
            portENTER_CRITICAL(&g_spinlock);
            g_system_halted = true;
            portEXIT_CRITICAL(&g_spinlock);

            // Update the report buffer with emergency stop message.
            if (xSemaphoreTake(g_report_mutex, portMAX_DELAY) == pdTRUE) {
                snprintf(g_report_buffer, sizeof(g_report_buffer), "SYSTEM HALTED!");
                ESP_LOGE(TAG, "=== !!! %s (Triggered on Core %d) !!! ===", g_report_buffer, xPortGetCoreID());
                xSemaphoreGive(g_report_mutex);
            }
        }
    }
}

static void init_hardware(void) {
    // Initialize LED.
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << PIN_LED_ALARM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_config);
    gpio_set_level(PIN_LED_ALARM, 0); // Ensure LED is off initially.

    // Initialize buttons.
    gpio_config_t btn_config = {
        .pin_bit_mask = (1ULL << PIN_BTN_COUNT) | (1ULL << PIN_BTN_STOP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&btn_config);

    // Install ISR service.
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BTN_COUNT, btn_count_isr_handler, NULL);
    gpio_isr_handler_add(PIN_BTN_STOP, btn_stop_isr_handler, NULL);

    // ADC initialization.
    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = LDR_ADC_UNIT,
    };
    adc_oneshot_new_unit(&adc_config, &g_adc_handle);

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT, // 12-bit resolution (0 - 4095 range).
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(g_adc_handle, LDR_ADC_CHANNEL, &chan_config);
}


void app_main(void) {
    // Create primitives before interupts and tasks launch.
    g_report_mutex = xSemaphoreCreateMutex();
    g_emergency_semaphore = xSemaphoreCreateBinary();

    if (g_report_mutex == NULL || g_emergency_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives. Exiting...");
        return;
    }

    init_hardware();

    // Task 1: Core 0, Priority 5 - Conveyor speed monitoring and reporting.
    xTaskCreatePinnedToCore(task_conveyor_speed, "ConveyorTask", 4096, NULL, 5, NULL, 0);

    // Task 2: Core 1, Priority 5 - ADC sensor reading and reporting.
    xTaskCreatePinnedToCore(task_adc_sensor, "AdcTask", 4096, NULL, 5, NULL, 1);

    // Task 3: Core 1, Priority 10 - Emergency stop handling.
    xTaskCreatePinnedToCore(task_emergency_halt, "EmergencyTask", 4096, NULL, 10, NULL, tskNO_AFFINITY);

    ESP_LOGI(TAG, "Conveyor system started! Press BTN1 to count parts, BTN2 for Emergency Stop.");
}
