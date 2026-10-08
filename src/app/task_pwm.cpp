/**
 * @file  task_pwm.cpp
 * @brief PWM 输出任务（GPIO5 / LEDC 通道2）
 *
 * 界面在按钮/文本框变化时通过 app_pwm_set() 下发设定值并唤醒本任务，
 * 任务只负责把设定值写进 LEDC，界面切走也不停。
 *
 * 按需运行：LEDC 一旦配好就由硬件自己持续输出，不需要 CPU 陪着轮询——
 * 平时阻塞在任务通知上（CPU 占用 0），设定值一变立刻醒来（比原来 50 ms 轮询更快），
 * 关闭输出时写一次占空比 0，然后继续阻塞。
 */
#include "app_shared.h"

#define PWM_PIN 5
#define PWM_CHANNEL 2

static void pwm_task(void *arg)
{
    (void)arg;
    bool applied = false;
    int  applied_freq = 0, applied_duty = 0;

    for (;;)
    {
        int freq = 0, duty = 0;
        bool on = app_pwm_get(&freq, &duty);

        if (on)
        {
            if (!applied || freq != applied_freq || duty != applied_duty)
            {
                int duty255 = duty * 255 / 100; /* 占空比 0-100% -> 0-255 */
                ledcAttachPin(PWM_PIN, PWM_CHANNEL);
                ledcSetup(PWM_CHANNEL, freq, 8);
                ledcWrite(PWM_CHANNEL, duty255);
                applied_freq = freq;
                applied_duty = duty;
                applied = true;
            }
        }
        else if (applied)
        {
            ledcWrite(PWM_CHANNEL, 0);
            applied = false;
        }

        /* 没有新设定值就阻塞：波形由硬件输出，任务不占 CPU */
        app_task_wait(APP_TASK_PWM, 0, false);
    }
}

void app_task_pwm_start(void)
{
    app_task_start(pwm_task, "pwm", 3072, 1, 0, APP_TASK_PWM);
}
