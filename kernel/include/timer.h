#pragma once
#include <stdint.h>
#include <stdbool.h>

void     timer_init(unsigned hz);
unsigned timer_hz(void);
uint64_t timer_ticks(void);

uint64_t timer_ms_to_ticks(uint64_t ms);

uint64_t clock_ns(void);
uint64_t timer_tsc_khz(void);
bool     timer_tsc_calibrated(void);
