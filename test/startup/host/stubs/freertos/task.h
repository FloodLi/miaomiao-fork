#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*run)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *out);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
