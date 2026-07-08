#include "timer.h"
#include "isr.h"
#include "../drivers/ports.h"

#define PIT_BASE_FREQUENCY  1193180
#define PIT_CHANNEL0        0x40
#define PIT_COMMAND         0x43
#define IRQ0_INTERRUPT      32

static volatile uint32_t tick = 0;

static void timer_callback(registers_t *regs) {
    (void)regs;
    tick++;
}

uint32_t get_tick(void) {
    return tick;
}

void timer_init(uint32_t frequency) {
    register_interrupt_handler(IRQ0_INTERRUPT, timer_callback);

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;

    // 0x36 = channel 0, access lo/hi byte, mode 3 (square wave generator).
    port_byte_out(PIT_COMMAND, 0x36);
    port_byte_out(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    port_byte_out(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}
