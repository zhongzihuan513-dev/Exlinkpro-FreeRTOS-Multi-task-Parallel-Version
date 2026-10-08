/**
 * @file  mp28167.cpp
 * @brief MP28167 数控电源驱动实现（I2C，地址与寄存器沿用 Exlink1.1 的实测值）
 */
#include "mp28167.h"
#include "app_shared.h"
#include <Arduino.h>
#include <Wire.h>

#define MP28167_ADDR 0x60 /* I2C 从机地址 */
#define REG_VREF_L   0x00 /* 参考电压低 3 位 */
#define REG_VREF_H   0x01 /* 参考电压高 8 位 */
#define REG_VREF_GO  0x02 /* bit0 = GO，置 1 让新参考电压生效 */

/* 单字节写：true = 从机 ACK */
static bool mp_write(uint8_t reg, uint8_t data)
{
    Wire.beginTransmission(MP28167_ADDR);
    Wire.write(reg);
    Wire.write(data);
    return Wire.endTransmission() == 0;
}

/* 单字节读 */
static bool mp_read(uint8_t reg, uint8_t *out)
{
    Wire.beginTransmission(MP28167_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false; /* 重复起始，不释放总线 */
    if (Wire.requestFrom((uint8_t)MP28167_ADDR, (uint8_t)1) != 1) return false;
    if (!Wire.available()) return false;
    *out = Wire.read();
    return true;
}

bool mp28167_read_vref(uint8_t *v_h, uint8_t *v_l, uint32_t timeout_ms)
{
    bool ok;
    if (!app_i2c_bus_lock(pdMS_TO_TICKS(timeout_ms))) return false;
    ok = mp_read(REG_VREF_H, v_h) && mp_read(REG_VREF_L, v_l);
    app_i2c_bus_unlock();
    return ok;
}

bool mp28167_set_voltage_mv(int mv, uint32_t timeout_ms)
{
    uint32_t code;
    uint8_t v_h, v_l, go = 0, h = 0, l = 0;
    bool ok;

    if (mv < 0) mv = 0;
    if (mv > MP28167_MAX_MV) mv = MP28167_MAX_MV;

    /* mV -> 11 位 VREF 码（Exlink1.1 的换算，未乘分压倍率） */
    code = ((uint32_t)mv * 100u) / 1039u;
    v_h  = (uint8_t)((code >> 3) & 0xFF);
    v_l  = (uint8_t)(code & 0x07);

    if (!app_i2c_bus_lock(pdMS_TO_TICKS(timeout_ms))) return false;

    ok = mp_write(REG_VREF_L, v_l);
    ok = ok && mp_write(REG_VREF_H, v_h);
    /* 置 GO 位让新参考电压生效 */
    if (ok && mp_read(REG_VREF_GO, &go)) ok = mp_write(REG_VREF_GO, (uint8_t)(go | 0x01));
    /* 回读校验：芯片无应答 / 地址不对时，串口立刻能看出来 */
    if (ok) ok = mp_read(REG_VREF_H, &h) && mp_read(REG_VREF_L, &l) && (h == v_h) && (l == v_l);

    app_i2c_bus_unlock();

    if (ok) Serial.printf("[mp28167] set %d mV (vref code %u = 0x%02X%01X)\n", mv, (unsigned)code, v_h, v_l);
    else    Serial.printf("[mp28167] set %d mV 失败（总线忙或芯片无应答，稍后重试）\n", mv);
    return ok;
}
