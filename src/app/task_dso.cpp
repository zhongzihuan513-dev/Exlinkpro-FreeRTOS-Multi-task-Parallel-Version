/**
 * @file  task_dso.cpp
 * @brief 简易示波器采样任务（ADC1_CH3 / GPIO4，12 位）
 *
 * RUN 时以约 1kHz 采样写入 256 点环形缓冲；界面定时器只负责取出新样本并画到
 * 图表上。采样独立于界面，切到别的功能页后波形继续采集。
 *
 * 按需运行：STOP（默认，开机即 STOP）时阻塞在任务通知上，完全不采样、不占 CPU；
 * 界面按 RUN 会立刻把它唤醒，按 STOP 也会立刻打断 1 kHz 节拍。
 */
#include "app_shared.h"

#define DSO_PIN 4

static void dso_task(void *arg)
{
    (void)arg;
    uint32_t last_stats = 0;

    for (;;)
    {
        if (app_dso_on())
        {
            app_dso_push_sample(analogRead(DSO_PIN));

            uint32_t now = millis();
            if (now - last_stats >= 50)
            {
                last_stats = now;
                app_dso_update_stats();
            }

            /* RUN：1 ms 采样节拍（被 app_dso_set(false) 打断时立刻回到阻塞分支） */
            app_task_wait(APP_TASK_DSO, 1, true);
        }
        else
        {
            /* STOP：阻塞，不采样也不占 CPU */
            app_task_wait(APP_TASK_DSO, 0, false);
        }
    }
}

void app_task_dso_start(void)
{
    app_task_start(dso_task, "dso", 3072, 2, 0, APP_TASK_DSO);
}
