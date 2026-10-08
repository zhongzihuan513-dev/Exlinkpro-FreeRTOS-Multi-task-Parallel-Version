/**
 * @file  app_shared.cpp
 * @brief Exlink3.0 共享状态实现：互斥量、流缓冲、功能开关与测量数据
 */
#include "app_shared.h"
#include "mp28167.h" /* MP28167_MAX_MV：输出电压上限 */
#include <string.h>
#include <stdio.h>

/* ---------- 与原 ui.c 共用的字符串 ---------- */
char voltageStr[20]  = "0.000";
char currentStr[20]  = "0.000";
char powerStr[20]    = "0.000";
char mAHStr[20]      = "0.000";
char maxValueStr[20] = "0.00";
char minValueStr[20] = "0.00";
char peakToPeakValueStr[20] = "0.00";
char freqencyStr[20] = "0";

/* ---------- 同步对象 ---------- */
static SemaphoreHandle_t s_lvgl_mux = NULL; /* LVGL 递归互斥 */
static SemaphoreHandle_t s_data_mux = NULL; /* 共享数据/字符串互斥 */
static SemaphoreHandle_t s_dso_mux  = NULL; /* DSO 环形缓冲互斥 */
static SemaphoreHandle_t s_i2c_mux  = NULL; /* Wire 总线互斥 */

static StreamBufferHandle_t s_uart_sb = NULL;
static StreamBufferHandle_t s_ble_sb  = NULL;
static StreamBufferHandle_t s_i2c_sb  = NULL;

/* ---------- 任务句柄 + 当前界面（按需唤醒用） ---------- */
/* 句柄会被 core 1（建任务）写、core 0（唤醒方）读，所以加 volatile */
static TaskHandle_t volatile s_task_handle[APP_TASK_COUNT] = {NULL};
static volatile uint32_t    s_task_pending = 0; /* 任务还没建就收到的唤醒，按位记账 */
static portMUX_TYPE         s_pend_mux = portMUX_INITIALIZER_UNLOCKED; /* 保护上面那张位图 */
static app_page_t           s_page = APP_PAGE_BOOT; /* 兼容旧接口：最近一次登记的页面 */
/* 多功能同屏：位 i = APP_PAGE_ 编号 i 的功能页当前开着（32 位写是原子操作） */
static volatile uint32_t    s_page_mask = 0;

/* ---------- 共享状态 ---------- */
static float s_v = 0.0f, s_a = 0.0f, s_w = 0.0f, s_mah = 0.0f, s_bat = 4.0f;
static volatile bool s_ina_ready = false;
static volatile bool s_out = false;
static volatile int  s_volt_mv = 0; /* MP28167 输出电压设定值（mV，0..20000）；开机 0 V */
static volatile int  s_preset = 0;  /* 0=手动 1=11V 2=5V 3=3V */

static volatile bool s_pwm_on = false;
static volatile int  s_pwm_freq = 0, s_pwm_duty = 0;

static volatile bool s_uart_on = false;
static volatile long s_uart_baud = 115200;

static volatile bool s_ble_on = false;
static volatile long s_ble_baud = 115200;
static volatile bool s_ble_conn = false;

static volatile bool s_i2c_req = false;
static volatile bool s_i2c_busy = false;

static volatile bool s_dso_on = false;
static volatile bool s_freq_on = false;
static volatile float s_freq = 0.0f;

static int32_t s_adc[APP_DSO_POINTS];
static volatile uint32_t s_adc_head = 0; /* 下一个写入位置 */
static volatile uint32_t s_adc_seq  = 0; /* 累计写入样本数 */

/* ================= 初始化 ================= */
void app_init(void)
{
    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    s_data_mux = xSemaphoreCreateMutex();
    s_dso_mux  = xSemaphoreCreateMutex();
    s_i2c_mux  = xSemaphoreCreateMutex();
    s_uart_sb  = xStreamBufferCreate(APP_STREAM_SIZE, 1);
    s_ble_sb   = xStreamBufferCreate(APP_STREAM_SIZE, 1);
    s_i2c_sb   = xStreamBufferCreate(APP_STREAM_SIZE, 1);
    for (int i = 0; i < APP_DSO_POINTS; i++) s_adc[i] = 2048;
}

void app_start_tasks(void)
{
    app_task_ui_start();
    app_task_power_start();
    app_task_ina_start();
    app_task_pwm_start();
    app_task_i2c_start();
    app_task_uart_start();
    app_task_ble_start();
    app_task_dso_start();
    app_task_freq_start();
}

/* ================= 按需唤醒 ================= */
void app_task_register(app_task_t t, TaskHandle_t handle)
{
    if ((int)t < 0 || t >= APP_TASK_COUNT) return;
    s_task_handle[t] = handle;
}

void app_task_wake(app_task_t t)
{
    if ((int)t < 0 || t >= APP_TASK_COUNT) return;
    TaskHandle_t h = s_task_handle[t];
    if (h)
    {
        xTaskNotifyGive(h); /* 计数不会丢：任务没在等也会被记下来 */
    }
    else
    {
        /* 任务还没建：先记账，别把这次唤醒吃掉（两个核可能同时置位，要上自旋锁） */
        portENTER_CRITICAL(&s_pend_mux);
        s_task_pending |= (1u << (int)t);
        portEXIT_CRITICAL(&s_pend_mux);
    }
}

bool app_task_start(TaskFunction_t fn, const char *name, uint32_t stack,
                    UBaseType_t prio, BaseType_t core, app_task_t t)
{
    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, &h, core) != pdPASS)
    {
        Serial.printf("[app] task %s create failed\n", name ? name : "?");
        return false;
    }
    app_task_register(t, h);
    return true;
}

uint32_t app_task_wait(app_task_t t, uint32_t period_ms, bool needed)
{
    /* 本任务在建立之前就收到过唤醒（例如 setup 里就改了开关）：先消费掉再睡 */
    if ((int)t >= 0 && t < APP_TASK_COUNT)
    {
        bool pending = false;
        portENTER_CRITICAL(&s_pend_mux);
        if (s_task_pending & (1u << (int)t))
        {
            s_task_pending &= ~(1u << (int)t);
            pending = true;
        }
        portEXIT_CRITICAL(&s_pend_mux);
        if (pending) return 1;
    }
    if (!needed)
    {
        /* 前端没调用这个功能：阻塞在这里，完全不占 CPU（被唤醒才重新判断需求） */
        return ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    if (period_ms == 0) period_ms = 1;
    /* 有需求：按周期睡，但状态一变就被 app_task_wake() 打断，响应不受周期限制 */
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period_ms));
}

/* ================= 当前界面 + 需求判定 ================= */
void app_debug_mem(const char *tag, int value)
{
    Serial.printf("[ui] %s (%d, free heap %u)\n", tag ? tag : "?", value,
                  (unsigned)ESP.getFreeHeap());
}

void app_ui_page_open(app_page_t page)
{
    if ((int)page < 0 || page >= APP_PAGE_COUNT) return;
    s_page_mask |= (1u << (int)page);
    s_page = page;
    /* INA 的需求跟界面走：DC POWER / Voltmeter 页一开要立刻全速采样 */
    app_task_wake(APP_TASK_INA);
}

void app_ui_page_close(app_page_t page)
{
    if ((int)page < 0 || page >= APP_PAGE_COUNT) return;
    s_page_mask &= ~(1u << (int)page);
    app_task_wake(APP_TASK_INA);
}

bool app_ui_page_is_open(app_page_t page)
{
    if ((int)page < 0 || page >= APP_PAGE_COUNT) return false;
    return (s_page_mask & (1u << (int)page)) != 0;
}

int app_ui_page_open_count(void)
{
    uint32_t m = s_page_mask;
    int n = 0;
    while (m)
    {
        n += (int)(m & 1u);
        m >>= 1;
    }
    return n;
}

void app_ui_page_set(app_page_t page)
{
    if ((int)page < 0 || page >= APP_PAGE_COUNT) return;
    s_page = page;
    /* 旧语义：只开这一页；主菜单/开机动画表示一个功能页都没开 */
    if (page == APP_PAGE_HOME || page == APP_PAGE_BOOT) s_page_mask = 0;
    else s_page_mask = (1u << (int)page);
    app_task_wake(APP_TASK_INA);
}

app_page_t app_ui_page_get(void) { return s_page; }

app_ina_mode_t app_ina_demand(void)
{
    if (s_out) return APP_INA_FULL; /* 输出打开：后台继续采样并累计 mAh */

    /* 新界面里主菜单始终可见（电池图标要显示），功能页与其同屏：
     *   - DC POWER / Voltmeter 页开着 -> 100 ms 全量采样（V/A/W/mAh + 波形）
     *   - 否则只要电池图标 -> 1 s 采一次电池（+ 输出关闭时不碰 I2C） */
    if (app_ui_page_is_open(APP_PAGE_POWER) || app_ui_page_is_open(APP_PAGE_VOLTMETER))
        return APP_INA_FULL;
    return APP_INA_BATTERY;
}

bool app_need_uart(void) { return s_uart_on || s_ble_on; }

/* ================= 锁 ================= */
bool app_lvgl_lock(TickType_t timeout)
{
    if (!s_lvgl_mux) return false;
    return xSemaphoreTakeRecursive(s_lvgl_mux, timeout) == pdTRUE;
}

void app_lvgl_unlock(void)
{
    if (s_lvgl_mux) xSemaphoreGiveRecursive(s_lvgl_mux);
}

bool app_i2c_bus_lock(TickType_t timeout)
{
    if (!s_i2c_mux) return false;
    return xSemaphoreTake(s_i2c_mux, timeout) == pdTRUE;
}

void app_i2c_bus_unlock(void)
{
    if (s_i2c_mux) xSemaphoreGive(s_i2c_mux);
}

/* ================= 运行状态 ================= */
bool app_func_running(app_func_t f)
{
    switch (f)
    {
    case FUNC_POWER: return s_out;
    case FUNC_PWM:   return s_pwm_on;
    case FUNC_UART:  return s_uart_on;
    case FUNC_I2C:   return s_i2c_busy;
    case FUNC_DSO:   return s_dso_on;
    case FUNC_BLE:   return s_ble_on;
    case FUNC_FREQ:  return s_freq_on;
    default:         return false;
    }
}

void app_status_text(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    static const struct { app_func_t f; const char *name; } items[] = {
        { FUNC_POWER, "PWR" }, { FUNC_PWM, "PWM" }, { FUNC_UART, "UART" },
        { FUNC_I2C, "SCAN" },  { FUNC_DSO, "DSO" }, { FUNC_BLE, "BLE" },
        { FUNC_FREQ, "FRE" },
    };
    size_t used = 0;
    int cnt = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++)
    {
        if (!app_func_running(items[i].f)) continue;
        int w = snprintf(buf + used, n - used, "%s%s", cnt ? " " : "", items[i].name);
        if (w <= 0 || (size_t)w >= (n - used)) break;
        used += (size_t)w;
        cnt++;
    }
    if (cnt == 0) snprintf(buf, n, "none");
}

/* ================= INA226 / 电池 ================= */
void app_get_ina(float *v, float *a, float *w, float *mah)
{
    if (!s_data_mux) return;
    xSemaphoreTake(s_data_mux, portMAX_DELAY);
    if (v) *v = s_v;
    if (a) *a = s_a;
    if (w) *w = s_w;
    if (mah) *mah = s_mah;
    xSemaphoreGive(s_data_mux);
}

void app_set_ina(float v, float a, float w, float mah)
{
    if (!s_data_mux) return;
    xSemaphoreTake(s_data_mux, portMAX_DELAY);
    s_v = v; s_a = a; s_w = w; s_mah = mah;
    if (v > 10.0f) snprintf(voltageStr, sizeof(voltageStr), "%0.2f", v);
    else           snprintf(voltageStr, sizeof(voltageStr), "%0.3f", v);
    if (a > 10.0f) snprintf(currentStr, sizeof(currentStr), "%0.2f", a);
    else           snprintf(currentStr, sizeof(currentStr), "%0.3f", a);
    if (w > 10.0f) snprintf(powerStr, sizeof(powerStr), "%0.2f", w);
    else           snprintf(powerStr, sizeof(powerStr), "%0.3f", w);
    snprintf(mAHStr, sizeof(mAHStr), "%0.3f", mah);
    xSemaphoreGive(s_data_mux);
}

bool app_ina_ready(void) { return s_ina_ready; }
void app_set_ina_ready(bool r) { s_ina_ready = r; }

float app_get_battery(void) { return s_bat; }
void  app_set_battery(float volts) { s_bat = volts; }

bool app_get_output(void) { return s_out; }
void app_set_output(bool on)
{
    if (on == s_out) return;
    s_out = on;
    app_task_wake(APP_TASK_POWER); /* 立即动作，不必等轮询 */
    app_task_wake(APP_TASK_INA);   /* 输出开关决定 mAh 是否继续累计 */
}

int  app_get_volt_mv(void) { return s_volt_mv; }

void app_set_volt_mv(int mv)
{
    if (mv < 0) mv = 0;
    if (mv > MP28167_MAX_MV) mv = MP28167_MAX_MV;
    if (mv == s_volt_mv) return;
    s_volt_mv = mv;
    s_preset  = 0;                  /* 手动输入/微调后不再算"预设档" */
    app_task_wake(APP_TASK_POWER);  /* 有新设定值：叫醒 power 任务去写 MP28167 */
}

void app_volt_step(int dir)
{
    app_set_volt_mv(s_volt_mv + (dir > 0 ? 100 : -100)); /* 每次 0.1 V */
}

void app_set_preset(int preset)
{
    int mv = s_volt_mv;
    s_preset = preset;
    if (preset == 1) mv = 11000;      /* 11.0 V */
    else if (preset == 2) mv = 5000;  /* 5.0 V  */
    else if (preset == 3) mv = 3000;  /* 3.0 V  */
    s_volt_mv = mv;
    app_task_wake(APP_TASK_POWER);
}

/* ================= PWM ================= */
void app_pwm_set(bool on, int freq, int duty)
{
    if (freq < 1) freq = 1;
    if (freq > 100000) freq = 100000;
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;
    if (on == s_pwm_on && freq == s_pwm_freq && duty == s_pwm_duty) return; /* 界面上有 300ms 同步定时器，值没变就别吵醒任务 */
    s_pwm_freq = freq;
    s_pwm_duty = duty;
    s_pwm_on = on;
    app_task_wake(APP_TASK_PWM);
}

bool app_pwm_get(int *freq, int *duty)
{
    if (freq) *freq = s_pwm_freq;
    if (duty) *duty = s_pwm_duty;
    return s_pwm_on;
}

/* ================= UART 助手 ================= */
void app_uart_set(bool on, long baud)
{
    bool changed = (on != s_uart_on);
    if (baud > 0 && baud != s_uart_baud)
    {
        s_uart_baud = baud;
        changed = true; /* 波特率变了也要立刻叫醒串口任务去重设 Serial */
    }
    s_uart_on = on;
    if (changed) app_task_wake(APP_TASK_UART);
}
bool app_uart_on(void) { return s_uart_on; }
long app_uart_baud(void) { return s_uart_baud; }

void app_uart_push(const char *txt)
{
    if (!s_uart_sb || !txt) return;
    xStreamBufferSend(s_uart_sb, txt, strlen(txt), 0);
}
size_t app_uart_read(char *buf, size_t n)
{
    if (!s_uart_sb || !buf || n == 0) return 0;
    return xStreamBufferReceive(s_uart_sb, buf, n, 0);
}

/* ================= BLE ================= */
void app_ble_set(bool on, long baud)
{
    bool changed = false;
    if (baud > 0 && baud != s_ble_baud)
    {
        s_ble_baud = baud;
        changed = true;
    }
    if (on != s_ble_on)
    {
        s_ble_on = on;
        changed = true;
        app_task_wake(APP_TASK_UART); /* 串口任务按 BLE 开关决定是否转发 */
    }
    if (changed) app_task_wake(APP_TASK_BLE);
}
bool app_ble_on(void) { return s_ble_on; }
long app_ble_baud(void) { return s_ble_baud; }
void app_ble_set_connected(bool c)
{
    if (c == s_ble_conn) return;
    s_ble_conn = c;
    app_task_wake(APP_TASK_BLE); /* 断线立刻重新广播，不用等 200ms 轮询 */
}
bool app_ble_connected(void) { return s_ble_conn; }

void app_ble_push(const char *txt)
{
    if (!s_ble_sb || !txt) return;
    xStreamBufferSend(s_ble_sb, txt, strlen(txt), 0);
}
size_t app_ble_read(char *buf, size_t n)
{
    if (!s_ble_sb || !buf || n == 0) return 0;
    return xStreamBufferReceive(s_ble_sb, buf, n, 0);
}

/* ================= I2C 扫描 ================= */
void app_i2c_scan_request(void)
{
    if (!s_i2c_busy)
    {
        s_i2c_req  = true;
        s_i2c_busy = true;
        app_task_wake(APP_TASK_I2C); /* 扫描任务平时阻塞着，有请求才起来 */
    }
}
bool app_i2c_take_request(void)
{
    if (!s_i2c_req) return false;
    s_i2c_req = false;
    return true;
}
void app_i2c_done(void) { s_i2c_busy = false; }
bool app_i2c_busy(void) { return s_i2c_busy; }

void app_i2c_push(const char *txt)
{
    if (!s_i2c_sb || !txt) return;
    xStreamBufferSend(s_i2c_sb, txt, strlen(txt), 0);
}
size_t app_i2c_read(char *buf, size_t n)
{
    if (!s_i2c_sb || !buf || n == 0) return 0;
    return xStreamBufferReceive(s_i2c_sb, buf, n, 0);
}

/* ================= DSO ================= */
void app_dso_set(bool on)
{
    if (on == s_dso_on) return;
    s_dso_on = on;
    app_task_wake(APP_TASK_DSO); /* RUN 就 1kHz 采样，STOP 就阻塞 */
}
bool app_dso_on(void) { return s_dso_on; }

void app_dso_push_sample(int raw)
{
    if (!s_dso_mux) return;
    if (xSemaphoreTake(s_dso_mux, portMAX_DELAY) == pdTRUE)
    {
        s_adc[s_adc_head] = raw;
        s_adc_head = (s_adc_head + 1) % APP_DSO_POINTS;
        s_adc_seq++;
        xSemaphoreGive(s_dso_mux);
    }
}

int app_dso_poll(uint32_t *seq, int *dst, int max)
{
    if (!s_dso_mux || !seq || !dst || max <= 0) return 0;
    int n = 0;
    xSemaphoreTake(s_dso_mux, portMAX_DELAY);
    uint32_t total = s_adc_seq;
    uint32_t have = total - *seq;
    if (have > APP_DSO_POINTS) have = APP_DSO_POINTS; /* 太旧的数据丢弃 */
    if ((int)have > max) have = (uint32_t)max;
    for (uint32_t i = 0; i < have; i++)
    {
        uint32_t idx = (s_adc_head + APP_DSO_POINTS - have + i) % APP_DSO_POINTS;
        dst[i] = s_adc[idx];
    }
    *seq = total;
    n = (int)have;
    xSemaphoreGive(s_dso_mux);
    return n;
}

void app_dso_stats(float *vmax, float *vmin, float *vpp)
{
    if (vmax) *vmax = 0.0f;
    if (vmin) *vmin = 0.0f;
    if (vpp)  *vpp = 0.0f;
    if (!s_dso_mux) return;
    int mx = 0, mn = 4095;
    xSemaphoreTake(s_dso_mux, portMAX_DELAY);
    for (int i = 0; i < APP_DSO_POINTS; i++)
    {
        int val = s_adc[i];
        if (val > mx) mx = val;
        if (val < mn) mn = val;
    }
    xSemaphoreGive(s_dso_mux);
    float fmax = mx / 4095.0f * 3.3f;
    float fmin = mn / 4095.0f * 3.3f;
    if (vmax) *vmax = fmax;
    if (vmin) *vmin = fmin;
    if (vpp)  *vpp = fmax - fmin;
}

void app_dso_update_stats(void)
{
    float fmax, fmin, vpp;
    app_dso_stats(&fmax, &fmin, &vpp);
    if (!s_data_mux) return;
    xSemaphoreTake(s_data_mux, portMAX_DELAY);
    snprintf(maxValueStr, sizeof(maxValueStr), "%0.2f", fmax);
    snprintf(minValueStr, sizeof(minValueStr), "%0.2f", fmin);
    snprintf(peakToPeakValueStr, sizeof(peakToPeakValueStr), "%0.2f", vpp);
    xSemaphoreGive(s_data_mux);
}

/* ================= 频率计 ================= */
void app_freq_set(bool on)
{
    if (on == s_freq_on) return;
    s_freq_on = on;
    if (!on)
    {
        s_freq = 0.0f;
        if (s_data_mux) /* STOP 后界面上的读数同步清零 */
        {
            xSemaphoreTake(s_data_mux, portMAX_DELAY);
            snprintf(freqencyStr, sizeof(freqencyStr), "0");
            xSemaphoreGive(s_data_mux);
        }
    }
    app_task_wake(APP_TASK_FREQ); /* 开关立刻生效，不必等下一轮轮询 */
}
bool app_freq_on(void) { return s_freq_on; }

void app_freq_update(int32_t hz)
{
    s_freq = (float)hz;
    if (!s_data_mux) return;
    xSemaphoreTake(s_data_mux, portMAX_DELAY);
    snprintf(freqencyStr, sizeof(freqencyStr), "%d", (int)hz);
    xSemaphoreGive(s_data_mux);
}
float app_freq_get(void) { return s_freq; }

/* ================= 字符串安全拷贝 ================= */
void app_copy_str(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0 || !src) return;
    if (!s_data_mux)
    {
        strncpy(dst, src, n - 1);
        dst[n - 1] = 0;
        return;
    }
    xSemaphoreTake(s_data_mux, portMAX_DELAY);
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    xSemaphoreGive(s_data_mux);
}
