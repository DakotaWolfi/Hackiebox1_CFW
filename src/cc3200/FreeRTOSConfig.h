/*
 * FreeRTOS configuration. The kernel tick is the system tick:
 * SysTick with a period of exactly 80 000 cycles (1 ms).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configCPU_CLOCK_HZ 80000000UL
#define configTICK_RATE_HZ 1000
/* 1 tick = 1 ms: the conversion is the identity (the kernel's default
 * multiplies in 64 bits and links a 64-bit division). */
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0 /* no sleep states */
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 128
#define configMAX_TASK_NAME_LEN 8
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configENABLE_BACKWARD_COMPATIBILITY 0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM 1
#define configSTACK_DEPTH_TYPE uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE 0

/* No heap: every object is allocated statically. */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1

#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 1 /* input debouncing (input.c) */
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0
#define configGENERATE_RUN_TIME_STATS 0
#define configUSE_TRACE_FACILITY 0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_QUEUE_SETS 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_CO_ROUTINES 0

#define configUSE_TIMERS 0 /* no software timers in use */
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH 8
#define configTIMER_TASK_STACK_DEPTH 256

/* The CC3200 implements 3 priority bits. */
#define configPRIO_BITS 3
#define configKERNEL_INTERRUPT_PRIORITY (7 << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (2 << (8 - configPRIO_BITS))

#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetHandle 1
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskPrioritySet 0
#define INCLUDE_uxTaskPriorityGet 0

/* Line only: the caller's code position names the file (rtos.c). */
void rtos_assert_failed(int line);
#define configASSERT(x)                   \
    do                                    \
    {                                     \
        if (!(x))                         \
            rtos_assert_failed(__LINE__); \
    } while (0)

/* The port's handlers are the entries of the vector table (startup.c). */
#define vPortSVCHandler svc_handler
#define xPortPendSVHandler pendsv_handler
#define xPortSysTickHandler systick_handler

#endif
