/**
 * @file  mp28167.h
 * @brief MP28167 数控电源驱动（ExlinkPro 板的调压芯片，替代原版 Exlink 的 MCP4017）
 *
 * MP28167 是 MPS 的四开关 buck-boost + I2C 接口电源芯片：输出电压由参考电压
 * VREF（11 位码，寄存器 0x00/0x01）决定，写完再置 0x02 的 GO 位生效。
 * 换算沿用 Exlink1.1 在 Pro 板上用过的公式：code = mV * 100 / 1039。
 *
 * 本模块只管"把一个毫伏值写进芯片"；总线互斥在模块内部处理
 * （MP28167 与触摸/INA226 共用 Wire，见 app_shared.h 的 app_i2c_bus_lock）。
 */
#ifndef _MP28167_H
#define _MP28167_H

#include <stdint.h>
#include <stdbool.h>

#define MP28167_MAX_MV 20000 /* 输出电压上限 20 V */

#ifdef __cplusplus
extern "C"
{
#endif

    /* 设定输出电压：mv 会被夹到 0..MP28167_MAX_MV；
     * timeout_ms 为等 I2C 总线锁的时间；返回 false = 总线忙或芯片无应答（调用方可重试）。 */
    bool mp28167_set_voltage_mv(int mv, uint32_t timeout_ms);

    /* 读回 VREF 寄存器（开机自检用：芯片在不在、地址对不对） */
    bool mp28167_read_vref(uint8_t *v_h, uint8_t *v_l, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
