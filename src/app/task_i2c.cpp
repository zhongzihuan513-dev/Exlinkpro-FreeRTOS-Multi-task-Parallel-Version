/**
 * @file  task_i2c.cpp
 * @brief I2C 总线扫描任务（GPIO7=SCL / GPIO8=SDA）
 *
 * 界面点 SCAN 只是发一个请求（app_i2c_scan_request）并唤醒本任务，扫描在任务里完成，
 * 结果写入文本流缓冲，界面定时器取出后显示；扫描期间界面依然可以操作别的功能。
 *
 * 按需运行：没有扫描请求时阻塞在任务通知上（CPU 占用 0），
 * 且第一次真正要扫描时才 Wire1.begin() 占用引脚——前端没调用就一点都不碰硬件。
 */
#include "app_shared.h"
#include <Wire.h>

static void i2c_task(void *arg)
{
    (void)arg;
    bool wire_begun = false;
    char line[64];

    for (;;)
    {
        /* 没有请求就一直睡：被 app_i2c_scan_request() 唤醒
         *（拿不到请求说明是一次"顺带"唤醒，继续等） */
        app_task_wait(APP_TASK_I2C, 0, false);
        if (!app_i2c_take_request()) continue;

        if (!wire_begun)
        {
            Wire1.begin(6, 7); /* 第一次扫描才占用 I2C 扫描引脚 */
            wire_begun = true;
        }

        app_i2c_push("Scanning...\n");
        int nDevices = 0;

        for (uint8_t address = 1; address < 127; address++)
        {
            Wire1.beginTransmission(address);
            uint8_t error = Wire1.endTransmission();

            if (error == 0)
            {
                snprintf(line, sizeof(line), "I2C device address 0x%02X\n", address);
                app_i2c_push(line);
                nDevices++;
            }
            else if (error == 4)
            {
                snprintf(line, sizeof(line), "Unknown error at address 0x%02X\n", address);
                app_i2c_push(line);
            }
            vTaskDelay(pdMS_TO_TICKS(1)); /* 让出 CPU，不影响界面刷新 */
        }

        app_i2c_push(nDevices == 0 ? "No I2C devices found\n" : "done\n");
        app_i2c_done();
    }
}

void app_task_i2c_start(void)
{
    app_task_start(i2c_task, "i2cscan", 4096, 1, 0, APP_TASK_I2C);
}
