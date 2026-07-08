#ifndef TIMER_H
#define TIMER_H

#include "../include/types.h"

void timer_init(uint32_t frequency);
uint32_t get_tick(void);

#endif
