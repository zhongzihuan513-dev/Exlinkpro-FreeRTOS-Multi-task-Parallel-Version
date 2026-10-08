/**
 * @file  task_freq.cpp
 * @brief 频率计任务（PCNT 单元0，输入 GPIO5）
 *
 * 注意：硬件上 PWM 输出与频率计输入共用 GPIO5，两个功能同时打开时
 * 频率计会测到自己的 PWM 输出（见 README 的硬件限制）。
 *
 * 按需运行：RUN 时每 125 ms 读一次硬件计数；STOP 时先暂停 PCNT 计数，
 * 然后阻塞在任务通知上（CPU 占用 0，硬件也不再空计数）。
 */
#include "app_shared.h"
#include "driver/pcnt.h"

#define FREQ_PIN 5
#define FREQ_PCNT_UNIT PCNT_UNIT_0
#define FREQ_PERIOD_MS 125 /* 每 125ms 读一次计数：count x 8 = Hz */

static void freq_task(void *arg)
{
    (void)arg;

    pcnt_config_t pcnt_config = {
        .pulse_gpio_num = FREQ_PIN,
        .ctrl_gpio_num = PCNT_PIN_NOT_USED,
        .lctrl_mode = PCNT_MODE_KEEP,
        .hctrl_mode = PCNT_MODE_KEEP,
        .pos_mode = PCNT_COUNT_INC,
        .neg_mode = PCNT_COUNT_DIS,
        .unit = FREQ_PCNT_UNIT,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcnt_config);
    pcnt_counter_pause(FREQ_PCNT_UNIT); /* 开机默认 STOP：先不让它计数 */
    pcnt_counter_clear(FREQ_PCNT_UNIT);

    int16_t count = 0;
    bool counting = false;

    for (;;)
    {
        if (app_freq_on())
        {
            if (!counting)
            {
                pcnt_counter_clear(FREQ_PCNT_UNIT);
                pcnt_counter_resume(FREQ_PCNT_UNIT);
                counting = true;
            }

            pcnt_get_counter_value(FREQ_PCNT_UNIT, &count);
            pcnt_counter_clear(FREQ_PCNT_UNIT);
            app_freq_update((int32_t)count * 8);
            app_task_wait(APP_TASK_FREQ, FREQ_PERIOD_MS, true);
        }
        else
        {
            if (counting)
            {
                pcnt_counter_pause(FREQ_PCNT_UNIT); /* STOP：硬件也停下 */
                pcnt_counter_clear(FREQ_PCNT_UNIT);
                counting = false;
            }
            app_task_wait(APP_TASK_FREQ, 0, false); /* 阻塞，不占 CPU */
        }
    }
}

void app_task_freq_start(void)
{
    app_task_start(freq_task, "freq", 3072, 2, 0, APP_TASK_FREQ);
}
