/**
 * @file  task_power.cpp
 * @brief DC POWER 硬件任务：输出开关（GPIO1）+ MP28167 数控电源（I2C 参考电压）
 *
 * Exlink3.1：调压芯片由原版 Exlink 的 MCP4017 数字电位器换成 ExlinkPro 板上的
 * MP28167（四开关 buck-boost，I2C 写 VREF，0-20V 线性可调），驱动见 mp28167.cpp。
 * 输出开关（GPIO1）与时序都没变：设定值一改，本任务立刻去写，写不进去就重试。
 *
 * 按需运行：本任务没有任何周期工作——平时阻塞在任务通知上（CPU 占用 0），
 * 只有 app_set_output()/app_volt_step()/app_set_volt_mv()/app_set_preset() 改变
 * 设定值时才醒来；写入若因总线忙失败，则在"待写入"状态下按 100 ms 重试（写成功才记账）。
 */
#include "app_shared.h"
#include "mp28167.h"
#include <Wire.h>

#define POWER_OUT_PIN 1

#define POWER_RETRY_MS 100 /* 电压写入失败时的重试周期 */

static void power_task(void *arg)
{
    (void)arg;
    pinMode(POWER_OUT_PIN, OUTPUT);
    digitalWrite(POWER_OUT_PIN, LOW);

    bool last_out = false;
    int  last_mv  = -1;      /* -1 表示还没写过：开机先同步一次输出设定值 */
    bool mv_pending = true;  /* 有未写入的电压设定值 */

    for (;;)
    {
        bool out = app_get_output();
        if (out != last_out)
        {
            digitalWrite(POWER_OUT_PIN, out ? HIGH : LOW);
            last_out = out;
        }

        /* 关输出时把电压也写 0 V：ExlinkPro 板上作者的固件（Exlink1.1）就是这么做的
           （它的电源按钮只改电压，GPIO1 一直拉低）。这样即使本板 GPIO1 不控输出，
           "OFF" 也一定没有输出；开输出再把输入框里的设定值写回去。 */
        int mv = app_get_output() ? app_get_volt_mv() : 0;
        if (mv != last_mv) mv_pending = true;

        /* mp28167_set_voltage_mv() 内部自己拿 I2C 总线锁，写失败返回 false */
        if (mv_pending && mp28167_set_voltage_mv(mv, 200))
        {
            last_mv = mv; /* 只有写成功才记账，否则下一轮重试 */
            mv_pending = false;
        }

        /* 有待写入的值才按 100 ms 重试；否则一直阻塞到下次设定值变化 */
        app_task_wait(APP_TASK_POWER, POWER_RETRY_MS, mv_pending);
    }
}

void app_task_power_start(void)
{
    app_task_start(power_task, "power", 3072, 2, 0, APP_TASK_POWER);
}
