/**
 * @file  app_shared.h
 * @brief Exlink3.0 应用层（C++ 侧）：同步对象、总线锁、任务入口
 *
 * 设计要点：
 *   每个功能一个独立 FreeRTOS 任务，开关互不影响、可同时运行；
 *   返回主菜单只关界面，功能继续在后台跑（多功能复用、使用不被打断）；
 *   后台常驻的是"功能状态"而不是轮询循环——前端没调用时任务阻塞，不占 CPU。
 */
#ifndef EXLINK_APP_SHARED_H
#define EXLINK_APP_SHARED_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include "app_api.h"

#define APP_DSO_POINTS  256   /* DSO 环形缓冲深度（256 点） */
#define APP_STREAM_SIZE 4096  /* 文本流缓冲大小（UART/BLE/I2C） */

/* ---------------- 生命周期 ---------------- */
void app_init(void);        /* 创建互斥量/流缓冲，须在 ui_init() 之前调用 */
void app_start_tasks(void); /* 创建全部 FreeRTOS 任务 */

/* ---------------- 任务登记 + "按需唤醒" ----------------
 * 前端没用到某个功能时，对应任务必须真正阻塞（不占 CPU），而不是 continue 轮询；
 * 前端一调用（开关/按钮/换界面），用 app_task_wake() 立刻把它叫醒。
 * 由于 xTaskNotifyGive() 的通知会被计数保存，即使任务当时正忙也不会丢事件。*/
typedef enum
{
    APP_TASK_POWER = 0, /* DC 输出开关 + MP28167 数控电源 */
    APP_TASK_INA,       /* INA226 功率监测 + 电池 */
    APP_TASK_PWM,       /* LEDC PWM 输出 */
    APP_TASK_I2C,       /* I2C 总线扫描 */
    APP_TASK_UART,      /* 串口助手（含 BLE 转发） */
    APP_TASK_BLE,       /* BLE 无线串口 */
    APP_TASK_DSO,       /* 示波器 ADC 采样 */
    APP_TASK_FREQ,      /* 频率计 PCNT */
    APP_TASK_COUNT
} app_task_t;

void app_task_register(app_task_t t, TaskHandle_t handle); /* 建任务后调用一次 */
void app_task_wake(app_task_t t);                          /* 状态变化：立刻唤醒 */

/* 建任务 + 登记句柄（两件事必须成对做，否则唤醒会落在空句柄上）：
 * 各任务入口的 "先建后登记" 顺序由这里统一保证，建失败会打印一行而不是静默不工作。*/
bool app_task_start(TaskFunction_t fn, const char *name, uint32_t stack,
                    UBaseType_t prio, BaseType_t core, app_task_t t);

/* 统一的"等活干"入口（任务循环末尾调用）：
 *   needed = true ：有需求——最多睡 period_ms，期间任何 app_task_wake() 都会立即返回；
 *   needed = false：没需求——无限期阻塞（CPU 占用为 0），直到被 app_task_wake() 唤醒。
 * 返回非 0 表示是被 app_task_wake() 提前叫醒的（状态可能已经变了）。*/
uint32_t app_task_wait(app_task_t t, uint32_t period_ms, bool needed);

/* ---------------- 需求判定：界面 + 功能开关共同决定 ---------------- */
typedef enum
{
    APP_INA_IDLE = 0, /* 前端没用到电压/电流/功率/电池：INA 任务阻塞 */
    APP_INA_BATTERY,  /* 只要主菜单的电池图标：1 s 采一次电池，不碰 I2C */
    APP_INA_FULL      /* DC POWER / Voltmeter 界面，或输出已打开：100 ms 全量采样 */
} app_ina_mode_t;

app_ina_mode_t app_ina_demand(void); /* INA226 + 电池任务的需求档位 */
bool app_need_uart(void);            /* 串口助手打开，或 BLE 打开（需要转发） */

/* ---------------- 锁 ---------------- */
bool app_lvgl_lock(TickType_t timeout);   /* LVGL 递归互斥（界面任务内部使用） */
void app_lvgl_unlock(void);
bool app_i2c_bus_lock(TickType_t timeout); /* Wire 总线互斥：INA226/触摸/MP28167 共用 */
void app_i2c_bus_unlock(void);

/* ---------------- 任务侧写入接口 ---------------- */
void app_set_ina(float v, float a, float w, float mah);
void app_set_ina_ready(bool r);
void app_set_battery(float volts);
void app_uart_push(const char *txt);
void app_ble_push(const char *txt);
void app_ble_set_connected(bool c);
void app_ble_notify(const char *data, size_t len);
void app_i2c_push(const char *txt);
bool app_i2c_take_request(void);
void app_i2c_done(void);
void app_dso_push_sample(int raw);
void app_dso_stats(float *vmax, float *vmin, float *vpp); /* app_dso_poll 在 app_api.h 里声明 */
void app_dso_update_stats(void);
void app_freq_update(int32_t hz);

/* ---------------- 各任务入口 ---------------- */
void app_task_ui_start(void);
void app_task_power_start(void);
void app_task_ina_start(void);
void app_task_pwm_start(void);
void app_task_i2c_start(void);
void app_task_uart_start(void);
void app_task_ble_start(void);
void app_task_dso_start(void);
void app_task_freq_start(void);

#endif /* EXLINK_APP_SHARED_H */
