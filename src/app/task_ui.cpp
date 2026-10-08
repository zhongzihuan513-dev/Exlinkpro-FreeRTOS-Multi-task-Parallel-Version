/**
 * @file  task_ui.cpp
 * @brief LVGL 界面任务（固定 core 1）
 *
 * 整个 LVGL 只允许在这个任务里访问（lv_timer_handler 内部会跑所有 lv_timer
 * 和事件回调）；其它任务只写共享数据/流缓冲，由界面定时器取出后刷新控件，
 * 这样就不存在 LVGL 的线程安全问题。
 *
 * 关于 CPU：屏幕一直在显示，所以本任务是唯一必须保持周期唤醒的任务
 *（5 ms 节拍 = 界面刷新与触摸响应）；其余 8 个功能任务在没有前端调用时
 * 全部阻塞在 ulTaskNotifyTake(portMAX_DELAY) 上，CPU 占用为 0。
 */
#include "app_shared.h"
#include <lvgl.h>

#define UI_PERIOD_MS 5

static void ui_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        if (app_lvgl_lock(portMAX_DELAY))
        {
            lv_timer_handler();
            app_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
    }
}

void app_task_ui_start(void)
{
    xTaskCreatePinnedToCore(ui_task, "lvgl", 8192, NULL, 3, NULL, 1);
}
