#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "sched.h"
#include "timer.h"

#define PIT_HZ      1193182ULL
#define PIT_CH0     0x40
#define PIT_CH2     0x42
#define PIT_CMD     0x43
#define PORT_B      0x61

#define TSC_SHIFT   22

static volatile uint64_t ticks;
static unsigned hz = 1000;
static uint64_t tick_ns = 1000000;

static bool     tsc_ok;
static uint64_t tsc_khz;
static uint64_t tsc_base;
static uint64_t tsc_mult;

// Asks the PIT (a timer chip from 1981) how long 10 ms is by running channel 2 one-shot for 11932 ticks at 1.193 MHz while counting TSC ticks, because that's somehow still how you find out how fast your multi-GHz CPU is. Returns TSC kHz, or 0 if the PIT never finished
static uint64_t calibrate_once(void)
{
    const uint16_t count = 11932;
    uint8_t b = inb(PORT_B);
    outb(PORT_B, (uint8_t)((b & ~0x03) | 0x00));
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2, count & 0xFF);
    outb(PIT_CH2, count >> 8);

    outb(PORT_B, (uint8_t)((b & ~0x03) | 0x01));
    uint64_t t0 = rdtsc();
    uint64_t guard = 0;
    while (!(inb(PORT_B) & 0x20)) {
        if (++guard > 20000000ULL) {
            outb(PORT_B, b & ~0x03);
            return 0;
        }
    }
    uint64_t t1 = rdtsc();
    outb(PORT_B, b & ~0x03);

    return (t1 - t0) * PIT_HZ / ((uint64_t)count * 1000);
}

static void tsc_calibrate(void)
{
    uint64_t best = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t k = calibrate_once();
        if (k && (!best || k < best))
            best = k;
    }
    if (best < 100000 || best > 10000000)
        return;
    tsc_khz = best;
    tsc_mult = (1000000ULL << TSC_SHIFT) / tsc_khz;
    tsc_base = rdtsc();
    tsc_ok = true;
}

static void timer_irq(void)
{
    ticks++;
    sched_tick();
}

void timer_init(unsigned freq)
{
    hz = freq;
    tick_ns = 1000000000ULL / freq;

    tsc_calibrate();

    uint32_t div = (uint32_t)((PIT_HZ + freq / 2) / freq);
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);

    irq_register(IRQ_TIMER, timer_irq);
    irq_unmask(IRQ_TIMER);
}

unsigned timer_hz(void)                 { return hz; }
uint64_t timer_ticks(void)              { return ticks; }
uint64_t timer_tsc_khz(void)            { return tsc_khz; }
bool     timer_tsc_calibrated(void)     { return tsc_ok; }

uint64_t timer_ms_to_ticks(uint64_t ms)
{
    return (ms * hz + 999) / 1000 + 1;
}

uint64_t clock_ns(void)
{
    if (tsc_ok) {
        unsigned __int128 p = (unsigned __int128)(rdtsc() - tsc_base) * tsc_mult;
        return (uint64_t)(p >> TSC_SHIFT);
    }
    return ticks * tick_ns;
}
