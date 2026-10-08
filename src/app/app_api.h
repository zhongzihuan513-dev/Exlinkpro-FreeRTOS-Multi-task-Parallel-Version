/**
 * @file  app_api.h
 * @brief Exlink3.0 应用层对外接口（C 语言可直接包含，供 ui.c / event.c 使用）
 *
 * 这里只放不依赖 C++ 的声明：ui.c、event.c 是 C 文件，通过本头调用任务层，
 * 真正的实现在 app_shared.cpp 与各 task_*.cpp 中（使用 extern "C" 链接）。
 */
#ifndef EXLINK_APP_API_H
#define EXLINK_APP_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ---------------- 功能编号（主菜单运行状态指示用） ---------------- */
typedef enum
{
    FUNC_POWER = 0, /* DC POWER：输出开关 + MP28167 数控电源 */
    FUNC_PWM,       /* PWM OUT */
    FUNC_UART,      /* UART HELPER */
    FUNC_I2C,       /* I2C SCAN（一次性） */
    FUNC_DSO,       /* Simple DSO */
    FUNC_BLE,       /* BLE 无线串口 */
    FUNC_FREQ,      /* FRE Count */
    FUNC_COUNT
} app_func_t;

/* ---------------- 当前打开的界面（任务"按需唤醒"的依据） ---------------- */
/* 任务只在被前端用到时才工作：
 *   ① 界面开/关会登记"哪些页面开着"（app_ui_page_open/close，见下），采样类任务据此判断有没有人看；
 *   ② 功能开关（app_*_set / app_set_output / app_i2c_scan_request）会立刻唤醒对应任务；
 * 两者都不成立时，任务阻塞在 ulTaskNotifyTake(portMAX_DELAY) 上，不消耗 CPU。
 *
 * Exlink3.0 的新界面（EXlink2.1 模型）允许**多个功能页同时开着**，
 * 所以"当前界面"不再是一个值而是一个集合：exlink_open_function() 登记 open、
 * exlink_close_function() 登记 close；app_ui_page_set() 保留给"只开这一页"的旧语义
 * （开机动画 / 主菜单 = 一个功能页都没开）。 */
typedef enum
{
    APP_PAGE_BOOT = 0,  /* 开机动画：任何功能都还没被用到 */
    APP_PAGE_HOME,      /* 主菜单（只有电池图标 + RUN 状态） */
    APP_PAGE_PINMAP,    /* Pin Map */
    APP_PAGE_POWER,     /* DC POWER：V/A/W/mAh + 波形 */
    APP_PAGE_PWM,       /* PWM OUT */
    APP_PAGE_UART,      /* UART HELPER */
    APP_PAGE_I2C,       /* I2C SCAN */
    APP_PAGE_VOLTMETER, /* Voltmeter：电压 + 波形 */
    APP_PAGE_DSO,       /* Simple DSO */
    APP_PAGE_BLE,       /* BLE UART */
    APP_PAGE_FREQ,      /* FRE Count */
    APP_PAGE_INFO,      /* Device INFO */
    APP_PAGE_COUNT
} app_page_t;

/* 多功能同屏：一个页面一个位，可同时打开多个 */
void app_ui_page_open(app_page_t page);   /* 功能页被打开（按键 -> 页面） */
void app_ui_page_close(app_page_t page);  /* 功能页被关闭（× -> 按键）   */
bool app_ui_page_is_open(app_page_t page);
int  app_ui_page_open_count(void);

/* 板子上自检用：串口打印 "[ui] <tag> (<value>, free heap <n>)"（ui.c 是 C 文件，
 * 用不了 Serial/ESP 这两个 C++ 对象，所以包一层） */
void app_debug_mem(const char *tag, int value);

/* 兼容旧接口：把页面集合整体设成"只有这一页"（APP_PAGE_HOME/BOOT = 全关） */
void       app_ui_page_set(app_page_t page);
app_page_t app_ui_page_get(void);

/* 运行状态 */
bool app_func_running(app_func_t f);
void app_status_text(char *buf, size_t n);

/* DC POWER / 电压表（INA226 + MP28167 数控电源） */
void  app_get_ina(float *v, float *a, float *w, float *mah);
bool  app_ina_ready(void);
float app_get_battery(void);
bool  app_get_output(void);
void  app_set_output(bool on);
int   app_get_volt_mv(void);      /* 输出电压设定值（mV，0..20000） */
void  app_volt_step(int dir);     /* 微调：±100 mV */
void  app_set_volt_mv(int mv);    /* 直接给值（界面输入框）/ 夹到 0..20000 */
void  app_set_preset(int preset); /* 1=11V 2=5V 3=3V */


/* PWM */
void app_pwm_set(bool on, int freq, int duty);
bool app_pwm_get(int *freq, int *duty);

/* UART 助手 */
void   app_uart_set(bool on, long baud);
bool   app_uart_on(void);
long   app_uart_baud(void);
size_t app_uart_read(char *buf, size_t n);

/* BLE 无线串口 */
void   app_ble_set(bool on, long baud);
bool   app_ble_on(void);
long   app_ble_baud(void);
bool   app_ble_connected(void);
size_t app_ble_read(char *buf, size_t n);

/* I2C 扫描 */
void   app_i2c_scan_request(void);
bool   app_i2c_busy(void);
size_t app_i2c_read(char *buf, size_t n);

/* 简易示波器 */
void app_dso_set(bool on);
bool app_dso_on(void);
int  app_dso_poll(uint32_t *seq, int *dst, int max);

/* 频率计 */
void  app_freq_set(bool on);
bool  app_freq_on(void);
float app_freq_get(void);

/* 与原 ui.c 兼容的测量字符串（读写都有锁保护，请用 app_copy_str 取值） */
extern char voltageStr[20];
extern char currentStr[20];
extern char powerStr[20];
extern char mAHStr[20];
extern char maxValueStr[20];
extern char minValueStr[20];
extern char peakToPeakValueStr[20];
extern char freqencyStr[20];
void app_copy_str(char *dst, size_t n, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* EXLINK_APP_API_H */
