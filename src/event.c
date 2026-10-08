/**
 * @file  event.c
 * @brief Exlink3.0 事件回调（新界面模型：菜单按键 <-> 功能页面）
 *
 * 与原固件一致的地方：功能开关只调 app_*_set()，后台任务继续跑；
 * 不一样的地方：返回不再重建整个主菜单，而是关掉"当前焦点所在的功能页"。
 */
#include <Arduino.h>
#include "ui.h"
#include "event.h"
#include "app/app_api.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static lv_coord_t touch_start_x;
static lv_coord_t touch_start_y;
static lv_coord_t swipe_dx_max;      /* 本次手势里最大的右滑位移 */
static bool swipe_tracking = false;  /* 手指是否还按着（一次按下只记一次起点） */
static uint32_t swipe_tick = 0;      /* 刚刚发生过右滑返回的时刻 */

/* 右滑返回后 LVGL 还会补发一次 CLICKED（水平滑动不触发滚动，LVGL 认为这是一次点击），
 * 菜单按键必须把它吃掉，否则"右滑关页面"会顺手把手指下那个功能页也打开 */
bool exlink_swipe_recent(void)
{
    return swipe_tick != 0 && lv_tick_elaps(swipe_tick) < 100;
}

/* ------------------------------------------------------------------ */
/* 开机动画                                                            */
/* ------------------------------------------------------------------ */

void anim_cb1(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

void anim_cb2(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

void anim_end_callback(lv_anim_t *a)
{
    (void)a;
    exlink_show_menu();
}

/* ------------------------------------------------------------------ */
/* 返回：长按 / 右滑 -> 关掉当前焦点所在的功能页                        */
/* ------------------------------------------------------------------ */

static bool dropdown_open(void)
{
    if (uart_list && lv_dropdown_is_open(uart_list)) return true;
    if (wireless_uart_list && lv_dropdown_is_open(wireless_uart_list)) return true;
    return false;
}

void exlink_back_attach(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, exlink_back_event_cb, LV_EVENT_ALL, NULL);
}

void exlink_back_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED)
    {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            /* 起点只在"这一次按下"的第一次记录：手指划过控件边界时 LVGL 会
               重新发 PRESSED，不能让它把已经走过的距离清零 */
            if (!swipe_tracking)
            {
                swipe_tracking = true;
                swipe_dx_max = 0;
                touch_start_x = p.x;
                touch_start_y = p.y;
                if (EXLINK_UI_DEBUG) exlink_log("[touch] press (%d,%d)\n", (int)p.x, (int)p.y);
            }
        }
    }
    else if (code == LV_EVENT_PRESSING)
    {
        /* 松手那一帧不一定采得到触摸点，所以过程中一直累计最大位移 */
        lv_indev_t *indev = lv_indev_get_act();
        if (!swipe_tracking || indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            if (p.x - touch_start_x > swipe_dx_max) swipe_dx_max = p.x - touch_start_x;
        }
    }
    else if (code == LV_EVENT_RELEASED)
    {
        lv_indev_t *indev = lv_indev_get_act();
        lv_coord_t dx, dy;
        if (!swipe_tracking) return;
        /* 旋钮 ENTER 的松手也会走到这里，它不是指针，不参与滑动判定 */
        if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        swipe_tracking = false;
        {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            dx = p.x - touch_start_x;
            if (dx > swipe_dx_max) swipe_dx_max = dx;
            dy = p.y - touch_start_y;
            /* 串口自检：dx/dy 一直是 0 → 触摸坐标在拖动过程中根本没变；
               scrollY 不变 → 菜单列没有滚动 */
            if (EXLINK_UI_DEBUG)
                exlink_log("[touch] release (%d,%d) dx=%d dy=%d scrollY=%d swipe=%d\n",
                           (int)p.x, (int)p.y, (int)dx, (int)dy,
                           panel ? (int)lv_obj_get_scroll_y(panel) : -999,
                           (swipe_dx_max > SWIPE_THRESHOLD && swipe_dx_max > LV_ABS(dy)) ? 1 : 0);
        }
        /* 右滑 = 返回（与原机 event_handler_back 一致）；
           要求水平位移明显大于垂直位移，免得和上下滚动菜单打架 */
        if (swipe_dx_max > SWIPE_THRESHOLD && swipe_dx_max > LV_ABS(dy))
        {
            swipe_tick = lv_tick_get();
            if (exlink_keyboard_visible()) exlink_keyboard_hide();
            else exlink_close_focused_function();
        }
    }
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        /* 旋钮长按（main.cpp 向屏幕发这个事件）或触摸长按 */
        if (exlink_keyboard_visible()) exlink_keyboard_hide();
        else exlink_close_focused_function();
    }
    else if (code == LV_EVENT_KEY)
    {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)
        {
            if (exlink_keyboard_visible()) exlink_keyboard_hide();
            else exlink_close_focused_function();
        }
        else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        {
            if (!exlink_keyboard_visible() && !dropdown_open()) lv_group_focus_next(lv_group_get_default());
        }
        else if (key == LV_KEY_LEFT || key == LV_KEY_UP)
        {
            if (!exlink_keyboard_visible() && !dropdown_open()) lv_group_focus_prev(lv_group_get_default());
        }
    }
}

/* ------------------------------------------------------------------ */
/* 菜单按键 <-> 功能页面                                               */
/* ------------------------------------------------------------------ */

void menu_btn_event_cb(lv_event_t *e)
{
    int idx;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (exlink_swipe_recent()) return; /* 这一次是右滑返回，不是点按 */
    idx = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)) - 1;
    if (idx >= 0) exlink_open_function(idx);
}

void exlink_tile_close_event_cb(lv_event_t *e)
{
    int idx;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (exlink_swipe_recent()) return; /* 右滑返回不该顺手点掉 × */
    idx = (int)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)) - 1;
    if (idx >= 0) exlink_close_function(idx);
}

/* ------------------------------------------------------------------ */
/* DC POWER                                                            */
/* ------------------------------------------------------------------ */

/* 把 app 层的电压设定值写回输入框：按 +/- 或预设键以后要能立刻看见新值 */
static void volt_set_show(void)
{
    char b[12];
    int mv;
    if (volt_set == NULL) return;
    mv = app_get_volt_mv();
    snprintf(b, sizeof(b), "%d.%d", mv / 1000, (mv % 1000) / 100);
    lv_textarea_set_text(volt_set, b);
}

void poweronbtn_event_cb(lv_event_t *e)
{
    bool on;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_get_output();
    if (on && volt_set)
    {
        /* 开输出时按输入框里的值设压（和 PWM 页读 FRE/DUTY 是同一个套路）；
           实际写 MP28167 由 power 任务在总线锁里做 */
        const char *s = lv_textarea_get_text(volt_set);
        if (s && *s) app_set_volt_mv((int)(atof(s) * 1000.0f + 0.5f));
    }
    app_set_output(on); /* 实际 GPIO 由 power 任务输出 */
    if (poweron_label)
    {
        lv_obj_set_style_text_color(poweron_label, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
    }
}

void VUPbtn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_volt_step(+1); /* +0.1 V */
    volt_set_show();
}

void VDOWNbtn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_volt_step(-1); /* -0.1 V */
    volt_set_show();
}

void V11btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_set_preset(1);
    volt_set_show();
}

void V5btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_set_preset(2);
    volt_set_show();
}

void V3btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_set_preset(3);
    volt_set_show();
}

/* ------------------------------------------------------------------ */
/* PWM                                                                 */
/* ------------------------------------------------------------------ */

void pwm_btn_event_cb(lv_event_t *e)
{
    bool on;
    int freq, dv;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_pwm_get(NULL, NULL);
    freq = fre ? atoi(lv_textarea_get_text(fre)) : 1000;
    dv = duty ? atoi(lv_textarea_get_text(duty)) : 50;
    app_pwm_set(on, freq, dv);
    if (pwm_btn) lv_obj_set_style_bg_color(pwm_btn, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
}

/* ------------------------------------------------------------------ */
/* I2C 扫描                                                            */
/* ------------------------------------------------------------------ */

void i2conbtn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_i2c_scan_request(); /* 异步扫描，界面不卡 */
    if (i2con) lv_obj_set_style_bg_color(i2con, lv_color_hex(0x00FF7F), 0);
}

/* ------------------------------------------------------------------ */
/* UART / BLE                                                          */
/* ------------------------------------------------------------------ */

void uart_btn_event_cb(lv_event_t *e)
{
    bool on;
    long baud;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_uart_on();
    baud = app_uart_baud();
    if (uart_list)
    {
        char b[10] = {0};
        lv_dropdown_get_selected_str(uart_list, b, sizeof(b));
        if (atol(b) > 0) baud = atol(b);
    }
    app_uart_set(on, baud);
    if (uart_btn) lv_obj_set_style_bg_color(uart_btn, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
}

void wireless_uart_btn_event_cb(lv_event_t *e)
{
    bool on;
    long baud;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_ble_on();
    baud = app_ble_baud();
    if (wireless_uart_list)
    {
        char b[10] = {0};
        lv_dropdown_get_selected_str(wireless_uart_list, b, sizeof(b));
        if (atol(b) > 0) baud = atol(b);
    }
    app_ble_set(on, baud);
    if (wireless_uart_btn)
        lv_obj_set_style_bg_color(wireless_uart_btn, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
}

/* 波特率下拉框：立刻把新波特率交给任务 */
void uart_list_event_cb(lv_event_t *e)
{
    char b[10] = {0};
    long baud;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_dropdown_get_selected_str(uart_list, b, sizeof(b));
    baud = atol(b);
    if (baud > 0) app_uart_set(app_uart_on(), baud);
}

void wireless_uart_list_event_cb(lv_event_t *e)
{
    char b[10] = {0};
    long baud;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_dropdown_get_selected_str(wireless_uart_list, b, sizeof(b));
    baud = atol(b);
    if (baud > 0) app_ble_set(app_ble_on(), baud);
}

/* ------------------------------------------------------------------ */
/* DSO / 频率计：独立 RUN/STOP（关掉页面仍在后台跑）                    */
/* ------------------------------------------------------------------ */

void dso_run_btn_event_cb(lv_event_t *e)
{
    bool on;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_dso_on();
    app_dso_set(on);
    if (dso_run_btn)
    {
        lv_obj_t *l = lv_obj_get_child(dso_run_btn, 0);
        lv_obj_set_style_bg_color(dso_run_btn, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
        if (l) lv_label_set_text(l, on ? "RUN" : "STOP");
    }
}

void freq_run_btn_event_cb(lv_event_t *e)
{
    bool on;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    on = !app_freq_on();
    app_freq_set(on);
    if (freq_run_btn)
    {
        lv_obj_t *l = lv_obj_get_child(freq_run_btn, 0);
        lv_obj_set_style_bg_color(freq_run_btn, lv_color_hex(on ? 0x00FF7F : 0xFF0000), 0);
        if (l) lv_label_set_text(l, on ? "RUN" : "STOP");
    }
}

/* ------------------------------------------------------------------ */
/* 数字键盘                                                            */
/* ------------------------------------------------------------------ */

void textarea_click_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    exlink_keyboard_show(lv_event_get_target(e));
}

void keyboard_event_handler(lv_event_t *e)
{
    lv_obj_t *kb;
    uint16_t id;
    const char *txt;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    kb = lv_event_get_target(e);
    id = lv_btnmatrix_get_selected_btn(kb);
    txt = lv_btnmatrix_get_btn_text(kb, id);
    if (txt && (strcmp(txt, LV_SYMBOL_KEYBOARD) == 0 || strcmp(txt, LV_SYMBOL_OK) == 0))
    {
        exlink_keyboard_hide();
    }
}

/* ------------------------------------------------------------------ */
/* 主菜单右列的焦点滑条                                                */
/* ------------------------------------------------------------------ */

/* 焦点定时器自己改滑条时不要反过来又去改焦点（否则两边每 300 ms 打架一次） */
static bool s_slider_sync = false;

void exlink_slider_set_syncing(bool on)
{
    s_slider_sync = on;
}

void slider_event_cb(lv_event_t *e)
{
    int idx;
    lv_obj_t *target;
    (void)e;
    if (s_slider_sync) return; /* 定时器同步，不是用户拖动 */
    if (slider == NULL) return;

    idx = 10 - lv_slider_get_value(slider);
    target = exlink_entry_focus_obj(idx);
    /* 焦点一变，group_focus_cb 会把选中的那一项滚进屏幕 */
    if (target) lv_group_focus_obj(target);
}
