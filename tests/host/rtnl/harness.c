#include <stdio.h>
#include <stdarg.h>
#include "rtnl.c"
int test_lock_held;
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return 0; }
uint8_t buildblock_slot(void) { return 2; }
void monitor_mark(activity_t a) { (void)a; }
uint32_t tick_ms(void) { return 123456; }
uint32_t clock_now(void) { return 1791776107; }
void vTaskDelay(TickType_t t) { (void)t; }
TaskHandle_t xTaskCreateStatic(void (*f)(void *), const char *n, uint32_t d, void *p, UBaseType_t pr, StackType_t *s, StaticTask_t *tc)
{
    (void)f;
    (void)n;
    (void)d;
    (void)p;
    (void)pr;
    (void)s;
    (void)tc;
    return 0;
}
void debug_printf(const char *f, ...) { (void)f; }
uint32_t net_session(void) { return 0; }
uint8_t net_state(void) { return 0; }
int16_t cloud_log_connect(uint32_t s)
{
    (void)s;
    return -1;
}
void cloud_sync_request(bool u) { (void)u; }
void cloud_log_closed(void) {}
bool cloud_log_yield_wanted(void) { return false; }
bool ext_hello_received(void) { return false; }
uint8_t remote_command(uint8_t c, const uint8_t *arg, unsigned n)
{
    (void)c;
    (void)arg;
    (void)n;
    return 0;
}
void remote_status(remote_status_t *st) { memset(st, 0, sizeof *st); }
bool remote_changed(void) { return false; }
int16_t sl_Send(int16_t s, const void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)b;
    (void)f;
    return n;
}
int16_t sl_Recv(int16_t s, void *b, int16_t n, int16_t f)
{
    (void)s;
    (void)b;
    (void)n;
    (void)f;
    return -11;
}
int16_t sl_Close(int16_t s)
{
    (void)s;
    return 0;
}
int16_t sl_SetSockOpt(int16_t s, int16_t l, int16_t o, const void *v, SlSocklen_t n)
{
    (void)s;
    (void)l;
    (void)o;
    (void)v;
    (void)n;
    return 0;
}
int main(void)
{
    log_init(); /* identity records 7146, 703, 704, 7305 */
    identity_pending = true;
    log_args_t a = {0};
    log_arg_num(&a, -5);
    log_arg_text(&a, NULL);
    log_fn(LOG_WARN, 37, 6911, 1, &a);
    uint8_t uid[8] = {0xB7, 6, 0xEE, 0x0E, 0x50, 3, 4, 0xE0};
    log_event(EVT_PLAYBACK_STARTED, uid);
    log_event(EVT_BIG_EAR, NULL);
    fwrite(buf, 1, used, stdout);
    return 0;
}
