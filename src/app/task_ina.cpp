/**
 * @file  task_ina.cpp
 * @brief INA226 功率监测 + 电池电压采集任务
 *
 * 按需运行（原来是"无条件 100 ms 常驻轮询"）：
 *   - DC POWER / Voltmeter 界面打开，或 DC 输出已打开 -> 100 ms 全量采样（继续累计 mAh）；
 *   - 只停在主菜单 -> 1 s 采一次电池（界面上的电池图标），完全不碰 I2C；
 *   - 前端没用到电压/电流/功率/电池 -> 阻塞，CPU 占用 0，也不占 I2C 总线。
 *
 * 模块查找同样只在有需求时进行（找不到就每 5 s 重试）：不阻塞开机，也不空转扫描。
 * mAh 按两次采样的真实时间间隔积分，不再依赖"循环正好 100 ms"这个假设。
 */
#include "app_shared.h"
#include "INA.h"

#define INA_PERIOD_FULL_MS    100  /* 界面/输出需要数据时的采样周期 */
#define INA_PERIOD_BATTERY_MS 1000 /* 只要电池图标时的采样周期 */
#define INA_RETRY_MS          5000 /* 没找到模块时的重试周期 */

static INA_Class ina;
static uint8_t ina_device = UINT8_MAX;

static bool ina_find(void)
{
    bool found226 = false;

    if (!app_i2c_bus_lock(pdMS_TO_TICKS(500))) return false;
    uint8_t found = ina.begin(10, 10000);
    for (uint8_t i = 0; i < found; i++)
    {
        if (strcmp(ina.getDeviceName(i), "INA226") == 0)
        {
            ina_device = i;
            ina.reset(ina_device);
            found226 = true;
            break;
        }
    }
    app_i2c_bus_unlock();
    return found226;
}

static void ina_configure(void)
{
    if (app_i2c_bus_lock(pdMS_TO_TICKS(500)))
    {
        ina.setAveraging(4, ina_device);
        ina.setBusConversion(8244, ina_device); /* 最大转换时间 8.244ms */
        ina.setShuntConversion(8244, ina_device);
        ina.setMode(INA_MODE_CONTINUOUS_BOTH, ina_device);
        app_i2c_bus_unlock();
    }
}

static void ina_task(void *arg)
{
    (void)arg;
    bool     ready    = false; /* INA226 已探测 + 已配置 */
    bool     was_full = false; /* 上一轮是不是全量采样（决定 mAh 能不能接着积分） */
    double   mah      = 0.0;
    uint32_t last_ms  = 0;

    for (;;)
    {
        app_ina_mode_t mode = app_ina_demand();

        if (mode == APP_INA_IDLE)
        {
            /* 前端没有用到：阻塞，等界面/输出开关把它唤醒 */
            was_full = false;
            app_task_wait(APP_TASK_INA, 0, false);
            continue;
        }

        /* 电池电压只走 ADC，不占 I2C：主菜单要图标就采，和模块在不在没关系 */
        app_set_battery(analogRead(2) / 4095.0f * 6.6f);

        if (mode == APP_INA_BATTERY)
        {
            was_full = false; /* 只采电池时不累计 mAh */
            app_task_wait(APP_TASK_INA, INA_PERIOD_BATTERY_MS, true);
            continue;
        }

        if (!ready)
        {
            if (ina_find())
            {
                ina_configure();
                ready = true;
                app_set_ina_ready(true);
                Serial.printf("[ina] INA226 found at device %u\n", ina_device);
            }
            else
            {
                Serial.println("[ina] no INA226 found, retry in 5s");
                app_task_wait(APP_TASK_INA, INA_RETRY_MS, true);
                continue;
            }
        }

        uint64_t vol = 0;
        int64_t  cur = 0;
        /* Wire 总线由触摸、INA226、MP28167 共用，必须加锁 */
        if (app_i2c_bus_lock(pdMS_TO_TICKS(200)))
        {
            vol = ina.getBusMilliVolts(ina_device);
            cur = ina.getBusMicroAmps(ina_device) / 5000;
            app_i2c_bus_unlock();
        }
        if (cur > 20 * 1000) cur = 0;

        float v = vol / 1000.0f;
        float a = cur / 1000.0f;
        float w = v * a;

        uint32_t now = millis();
        if (!was_full)
        {
            last_ms  = now; /* 刚从阻塞/电池档回来：不补算中间那段电量 */
            was_full = true;
        }
        else
        {
            mah += (double)a * ((now - last_ms) / 1000.0) / 3.6; /* A x s / 3.6 = mAh */
            last_ms = now;
        }

        app_set_ina(v, a, w, (float)mah);
        app_task_wait(APP_TASK_INA, INA_PERIOD_FULL_MS, true);
    }
}

void app_task_ina_start(void)
{
    app_task_start(ina_task, "ina226", 4096, 2, 0, APP_TASK_INA);
}
