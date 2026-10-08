/**
 * @file  event.h
 * @brief Exlink3.0 事件回调（与 EXlink2.1 的交互模型一致）
 *
 * 输入设备：
 *   - 旋钮（LV_INDEV_TYPE_KEYPAD）：右转 = LV_KEY_NEXT，左转 = LV_KEY_PREV，
 *     按下 = LV_KEY_ENTER，长按 = 向当前屏幕发 LV_EVENT_LONG_PRESSED（见 main.cpp）；
 *   - 触摸：点菜单按键 = 打开功能页，点 × = 关掉功能页，右滑/长按 = 关掉当前功能页。
 *
 * 焦点（旋钮）的单位是"整个菜单项"：功能没开时是菜单按键，开了以后是整个页面，
 * 页面内部的控件不进焦点组（它们只用触摸操作）。
 */
#ifndef _EVENT_H
#define _EVENT_H

#include "ui.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SWIPE_THRESHOLD 30 /* 右滑返回的判定距离 */

    /* 开机动画 */
    void anim_cb1(void *obj, int32_t v);
    void anim_cb2(void *obj, int32_t v);
    void anim_end_callback(lv_anim_t *a);

    /* 触摸长按 / 右滑 / 旋钮 ESC：挂到屏幕、菜单按键、页面与页面控件上 */
    void exlink_back_attach(lv_obj_t *obj);
    void exlink_back_event_cb(lv_event_t *e);

    /* 刚刚是否发生了"右滑返回"（右滑后 LVGL 还会补一次 CLICKED，菜单按键/× 要忽略它） */
    bool exlink_swipe_recent(void);

    /* 菜单按键 + 页面右下角的 × */
    void menu_btn_event_cb(lv_event_t *e);
    void exlink_tile_close_event_cb(lv_event_t *e);

    /* 各功能页面的控件 */
    void poweronbtn_event_cb(lv_event_t *e);
    void pwm_btn_event_cb(lv_event_t *e);
    void i2conbtn_event_cb(lv_event_t *e);
    void uart_btn_event_cb(lv_event_t *e);
    void wireless_uart_btn_event_cb(lv_event_t *e);
    void uart_list_event_cb(lv_event_t *e);
    void wireless_uart_list_event_cb(lv_event_t *e);
    void VUPbtn_event_cb(lv_event_t *e);
    void VDOWNbtn_event_cb(lv_event_t *e);
    void V11btn_event_cb(lv_event_t *e);
    void V5btn_event_cb(lv_event_t *e);
    void V3btn_event_cb(lv_event_t *e);
    void dso_run_btn_event_cb(lv_event_t *e);
    void freq_run_btn_event_cb(lv_event_t *e);

    /* 数字键盘 */
    void textarea_click_event_cb(lv_event_t *e);
    void keyboard_event_handler(lv_event_t *e);

    /* 主菜单右列的焦点滑条（ui.c 的定时器同步滑条时用来屏蔽回调） */
    void slider_event_cb(lv_event_t *e);
    void exlink_slider_set_syncing(bool on);

#ifdef __cplusplus
}
#endif

#endif
