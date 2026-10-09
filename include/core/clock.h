#ifndef SSD_CORE_CLOCK_H
#define SSD_CORE_CLOCK_H

#include <stdint.h>

/*
 * 虚拟时钟：整个模拟器唯一的时间源，单位 ns。
 * 不使用真实时间，因此可以在几秒内跑完"写满数遍盘"的寿命仿真。
 *
 * S0 只提供时钟本体；事件队列（S1 介质模型的异步完成）后续在此扩展。
 */

void     ssd_clock_init(void);
uint64_t ssd_clock_now(void);
void     ssd_clock_set(uint64_t abs_ns);
void     ssd_clock_advance(uint64_t delta_ns);

#endif /* SSD_CORE_CLOCK_H */
