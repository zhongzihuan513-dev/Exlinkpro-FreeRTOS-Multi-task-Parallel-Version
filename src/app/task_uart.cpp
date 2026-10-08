/**
 * @file  task_uart.cpp
 * @brief 串口助手任务：读取 USB 串口，写入流缓冲（界面显示），
 *        BLE 打开并已连接时同时转发到无线串口。
 *
 * 波特率切换、开关都不再需要界面在场，切到别的功能页也继续收数据。
 *
 * 按需运行：串口助手和 BLE 都没打开时阻塞在任务通知上（CPU 占用 0），
 * 不再每 20 ms 空转轮询 Serial；打开后仍按 20 ms 收数据，
 * 开关/波特率一变会立刻打断等待去处理。
 */
#include "app_shared.h"

#define UART_PERIOD_MS 20

static void uart_task(void *arg)
{
    (void)arg;
    long cur_baud = 115200;

    for (;;)
    {
        bool uart_on = app_uart_on();
        bool ble_on  = app_ble_on();

        if (app_need_uart())
        {
            long baud = app_uart_baud();
            if (baud > 0 && baud != cur_baud)
            {
                /* 只改波特率，不 end()/begin() 拆装驱动：避免与其它任务的 Serial 日志打架 */
                Serial.updateBaudRate(baud);
                cur_baud = baud;
            }

            if (Serial.available() > 0)
            {
                String input = Serial.readStringUntil('\n');
                input += "\n";

                if (uart_on) app_uart_push(input.c_str());
                if (ble_on && app_ble_connected()) app_ble_notify(input.c_str(), input.length());
            }

            app_task_wait(APP_TASK_UART, UART_PERIOD_MS, true);
        }
        else
        {
            /* 前端没打开串口助手/BLE：阻塞，不空转 */
            app_task_wait(APP_TASK_UART, 0, false);
        }
    }
}

void app_task_uart_start(void)
{
    app_task_start(uart_task, "uart", 4096, 2, 0, APP_TASK_UART);
}
