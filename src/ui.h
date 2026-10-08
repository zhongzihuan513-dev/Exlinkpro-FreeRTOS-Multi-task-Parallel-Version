#ifndef _SQUARELINE_PROJECT_UI_H
#define _SQUARELINE_PROJECT_UI_H

#ifdef __cplusplus
extern "C"
{
#endif

#if defined __has_include
#if __has_include("lvgl.h")
#include "lvgl.h"
#elif __has_include("lvgl/lvgl.h")
#include "lvgl/lvgl.h"
#else
#include "lvgl.h"
#endif
#else
#include "lvgl.h"
#endif

    /* ==================================================================
     * Exlink3.0 界面（与 EXlink2.1 的界面模型一致）
     *
     * 主菜单的 10 个功能键不再"整屏切换"：点一下某个功能键，它自己就地变成
     * 该功能的功能页面（宽 = 菜单列宽，高 = 内容自适应），页面右下角有一个
     * × 按键可关掉它并恢复菜单按键。因此多个功能可以同时开在屏幕上，
     * 后台任务照常在跑（app/app_api.h 的开关与数据通道都不受影响）。
     * ================================================================== */

    /* ---- 主菜单 ---- */
    extern lv_obj_t *panel;         /* 左列：菜单按键 / 功能页面共用的容器 */
    extern lv_obj_t *slider;        /* 右列：焦点指示滑条 */
    extern lv_obj_t *bat_label;     /* 右列：电池图标 */
    extern lv_obj_t *status_label;  /* 右列：RUN n（后台运行中的功能数量） */
    extern lv_obj_t *status_detail; /* 右列：运行中的功能名列表 */
    extern lv_timer_t *slider_update_timer;
    extern lv_timer_t *menu_status_timer;

    /* ---- 各功能页面上的控件（供事件回调与搬运定时器使用） ---- */
    extern lv_obj_t *poweron_label;
    extern lv_obj_t *volt_set; /* DC POWER 页：输出电压设定值输入框（Exlink3.1） */
    extern lv_obj_t *volt_chart;
    extern lv_obj_t *cur_chart;
    extern lv_obj_t *fre;
    extern lv_obj_t *duty;
    extern lv_obj_t *pwm_btn;
    extern lv_obj_t *uart_btn;
    extern lv_obj_t *uart_list;
    extern lv_obj_t *uart_extarea;
    extern lv_obj_t *i2con;
    extern lv_obj_t *i2c_extarea;
    extern lv_obj_t *wireless_uart_btn;
    extern lv_obj_t *wireless_uart_list;
    extern lv_obj_t *wireless_uart_extarea;
    extern lv_obj_t *DSO_chart;
    extern lv_chart_series_t *DSO_ser;
    extern lv_obj_t *dso_run_btn;
    extern lv_obj_t *FRE_label;
    extern lv_obj_t *freq_run_btn;

    /* ---- 界面侧的"搬运"定时器（随页面开关，见 exlink_page_timers） ---- */
    extern lv_timer_t *updatelabel_timer1;
    extern lv_timer_t *updatelabel_timer2;
    extern lv_timer_t *updatelabel_timer3;
    extern lv_timer_t *adddata_timer;
    extern lv_timer_t *adddata_timer2;
    extern lv_timer_t *DSO_update_timer1;
    extern lv_timer_t *DSO_update_timer2;
    extern lv_timer_t *DSO_update_timer3;
    extern lv_timer_t *FRE_label_update_timer;
    extern lv_timer_t *uart_rx_timer;
    extern lv_timer_t *ble_rx_timer;
    extern lv_timer_t *i2c_rx_timer;
    extern lv_timer_t *pwm_sync_timer;
    extern lv_timer_t *dso_poll_timer;

    /* 触摸/旋钮输入设备（main.cpp 注册，ui.c 里挂到焦点组上） */
    extern lv_indev_t *indev_keypad;

    /* 后台任务写入的测量字符串（读的时候用 app_copy_str 加锁取快照） */
    extern char voltageStr[20], currentStr[20], powerStr[20], mAHStr[20];
    extern char maxValueStr[20], minValueStr[20], peakToPeakValueStr[20];
    extern char freqencyStr[20];

#define SAMPLE_COUNT 256

    LV_IMG_DECLARE(ui_img_game3_png);
    LV_IMG_DECLARE(Exlink_png);
    LV_IMG_DECLARE(pinmap_png);
    LV_IMG_DECLARE(power_png);
    LV_IMG_DECLARE(pwm_png);
    LV_IMG_DECLARE(usarthelper_png);
    LV_IMG_DECLARE(i2c_png);
    LV_IMG_DECLARE(voltmeter_png);
    LV_IMG_DECLARE(DSO_png);
    LV_IMG_DECLARE(wireless_png);
    LV_IMG_DECLARE(readme_png);
    LV_IMG_DECLARE(pwmint_png);
    LV_IMG_DECLARE(FREcounter_png);
    LV_IMG_DECLARE(kobe_png);

    /* ---- 功能页编号（与 app_api.h 的 app_page_t 顺序一致） ---- */
    enum
    {
        EXLINK_FN_PINMAP = 0,
        EXLINK_FN_POWER,
        EXLINK_FN_PWM,
        EXLINK_FN_UART,
        EXLINK_FN_I2C,
        EXLINK_FN_VOLTMETER,
        EXLINK_FN_DSO,
        EXLINK_FN_BLE,
        EXLINK_FN_FRE,
        EXLINK_FN_INFO,
        EXLINK_FN_COUNT
    };

    /* C 文件里可用的串口打印 / 读一个字节（-1 = 无数据），main.cpp 实现 */
    void exlink_log(const char *fmt, ...);
    int  exlink_serial_read(void);

    /* 界面自检开关：置 0 就完全不打印（[geom] 焦点/滚动信息、[touch] 触摸轨迹） */
#ifndef EXLINK_UI_DEBUG
#define EXLINK_UI_DEBUG 1
#endif

    /* ---- 界面入口 ---- */
    void ui_init(void);               /* 主题 + 开机动画 -> 主菜单 */
    void create_boot_animation(void);
    void exlink_show_menu(void);      /* 重建菜单（所有功能页关闭） */
    void ui_Screen1_screen_init(void);/* = exlink_show_menu（兼容旧调用） */

    /* ---- 菜单按键 <-> 功能页面 ---- */
    bool exlink_function_is_open(int idx);
    void exlink_open_function(int idx);  /* 按键 -> 该功能的页面 */
    void exlink_close_function(int idx); /* × -> 恢复菜单按键 */
    void exlink_close_all_functions(void);
    int exlink_open_function_count(void);
    lv_obj_t *exlink_function_tile(int idx);
    lv_obj_t *exlink_entry_focus_obj(int idx);
    int exlink_entry_of_obj(lv_obj_t *obj); /* 对象属于哪个菜单项（-1 = 不属于） */

    /* ---- 数字键盘浮层（PWM 的 FRE/DUTY 输入） ---- */
    void exlink_keyboard_show(lv_obj_t *textarea);
    void exlink_keyboard_hide(void);
    bool exlink_keyboard_visible(void);

    /* 退出触摸/长按时调用：关闭当前焦点所在的功能页 */
    void exlink_close_focused_function(void);

    /* ---- 串口自检（对应 EXlink2.2 的 exlink_dump_menu_geometry）----
     * 打印菜单列的几何、可滚动余量、当前焦点项是否完整可见。
     * 焦点一变就打印一行，插上串口就能看出"波轮/滑屏到底有没有让菜单列滚起来"。 */
    void exlink_dump_menu_geometry(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
