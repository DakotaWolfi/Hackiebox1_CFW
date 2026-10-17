#pragma once
/* The mutex stub counts takes, so a test can check what runs under it. */
typedef void *SemaphoreHandle_t;
typedef struct
{
    int x;
} StaticSemaphore_t;
extern int test_lock_held;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
static inline long xSemaphoreTake(SemaphoreHandle_t s, uint32_t t)
{
    (void)s;
    (void)t;
    test_lock_held++;
    return 1;
}
static inline long xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    test_lock_held--;
    return 1;
}
