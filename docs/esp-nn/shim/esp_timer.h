#ifndef ESP_TIMER_SHIM_H
#define ESP_TIMER_SHIM_H
#include <time.h>
#include <stdint.h>
static inline int64_t esp_timer_get_time(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (int64_t)ts.tv_sec*1000000LL+ts.tv_nsec/1000;}
#endif
