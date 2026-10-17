#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t StackType_t;
typedef struct
{
    int x;
} StaticTask_t;
typedef struct
{
    int x;
} StaticQueue_t;
typedef void *TaskHandle_t;
typedef struct hq *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(x) (x)
#define taskENTER_CRITICAL()
#define taskEXIT_CRITICAL()
void vTaskDelay(TickType_t t);
TaskHandle_t xTaskCreateStatic(void (*f)(void *), const char *n, uint32_t d, void *p, UBaseType_t pr, StackType_t *s, StaticTask_t *t);
QueueHandle_t xQueueCreateStatic(UBaseType_t len, UBaseType_t size, uint8_t *buf, StaticQueue_t *q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
#define taskENTER_CRITICAL_FROM_ISR() 0
#define taskEXIT_CRITICAL_FROM_ISR(m) ((void)(m))
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
