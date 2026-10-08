/**
 * @file  ui.c
 * @brief Exlink3.0 界面：主菜单按键 <-> 功能页面（就地替换，可多页同屏）
 *
 * 与 EXlink2.0 的"点一个功能整屏切换"不同，本界面采用 EXlink2.1 的模型：
 *
 *   +--------------------------+          +--------------------------+
 *   | [ Pin Map         icon ] |  点击    | [ Pin Map 页面      x ]  |
 *   | [ DC POWER        icon ] |  ---->   | [ DC POWER        icon ] |
 *   | [ PWM OUT         icon ] |          | [ PWM OUT 页面      x ]  |
 *   +--------------------------+          +--------------------------+
 *
 *   - 功能页面宽度 = 菜单列宽（LV_PCT(100)），高度 = 内容自适应（LV_SIZE_CONTENT）；
 *   - 每个页面右下角有 × 按键，关掉它菜单按键就回来了；
 *   - 可以同时开多个页面，后台功能任务照常在跑（与 app/app_api.h 的功能开关解耦）；
 *   - 焦点（旋钮/方向键/滑条）的单位是"整个菜单项"：功能没开时是菜单按键，
 *     开了以后是整个页面，不会选中页面内部的子控件（子控件用触摸点击）。
 *
 * 与后端（src/app）的接口只有 app_api.h：开关（app_*_set）、数据（app_get_ina /
 * app_*_read / app_dso_poll …）与"哪一页开着"（app_ui_page_open/close）。
 * 后台任务从不接触控件，界面侧的搬运定时器随页面开关创建/删除。
 */

#include <Arduino.h>
#include "ui.h"
#include "event.h"
#include "app/app_api.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* 颜色                                                                */
/* ------------------------------------------------------------------ */
#define COL_RED     0xFF0000
#define COL_GREEN   0x00FF7F
#define COL_CYAN    0x00FFFF
#define COL_GOLD    0xFFD700
#define COL_BLUE    0x1E90FF
#define COL_GRAY    0x808080
#define COL_TILEBG  0x0A0A0A
#define COL_RUNBG   0x0B3D0B
#define COL_CHARTBG 0x303030
#define COL_GRID    0x696969

/* ------------------------------------------------------------------ */
/* 全局控件（ui.h 里声明，事件回调与搬运定时器使用）                    */
/* ------------------------------------------------------------------ */
lv_obj_t *panel = NULL;
lv_obj_t *slider = NULL;
lv_obj_t *bat_label = NULL;
lv_obj_t *status_label = NULL;
lv_obj_t *status_detail = NULL;
lv_timer_t *slider_update_timer = NULL;
lv_timer_t *menu_status_timer = NULL;

/* ------------------------------------------------------------------ */
/* 整幅 UI 的水平微调                                                  */
/* ------------------------------------------------------------------ */
/* 整个界面（开机动画、菜单列、右列、数字键盘）都挂在这个根容器下面。
 * 如果屏幕（面板）装偏导致"整幅 UI 偏向一边"，只需要把 UI_X_SHIFT 改成
 * 要补偿的像素数：正数 = 整个界面向右移，负数 = 向左移。0 = 不偏移（同 3.0）。 */
#define UI_X_SHIFT 20

static lv_obj_t *ui_root = NULL;

static lv_obj_t *ui_root_create(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, 320, 240);
    lv_obj_set_pos(r, UI_X_SHIFT, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE); /* 空白处的事件仍落到屏幕上（右滑/长按返回要用） */
    return r;
}

static lv_obj_t *ui_root_prepare(void)
{
    if (ui_root == NULL) ui_root = ui_root_create(lv_scr_act());
    return ui_root;
}

#if EXLINK_UI_DEBUG
/* 串口实时微调界面水平位置（只在 UI 任务里跑，200 ms 一次）：
 *   发  x 18     -> 直接设成 18 像素
 *   发  + / -    -> 当前值 +1 / -1（一次一行，回车结束）
 * 调好后把 EXLINK_UI_DEBUG 改成 0（或把数值告诉开发者写进 UI_X_SHIFT）即可。 */
static void ui_shift_poll(void)
{
    static char buf[16];
    static uint8_t n = 0;

    int ch;
    while ((ch = exlink_serial_read()) >= 0)
    {
        char c = (char)ch;
        if (c == '\n' || c == '\r')
        {
            buf[n] = 0;
            if (n && ui_root)
            {
                int cur = (int)lv_obj_get_x(ui_root);
                if (buf[0] == '+') cur += 1;
                else if (buf[0] == '-') cur -= 1;
                else if (buf[0] == 'x' || buf[0] == 'X') cur = atoi(buf + 1);
                else cur = atoi(buf);
                lv_obj_set_x(ui_root, cur);
                exlink_log("[shift] UI_X_SHIFT = %d\n", cur);
            }
            n = 0;
        }
        else if (n < sizeof(buf) - 1) buf[n++] = c;
    }
}
#endif

lv_obj_t *poweron_label = NULL;
lv_obj_t *volt_set = NULL; /* DC POWER 页：输出电压设定值输入框（Exlink3.1 / MP28167） */
lv_obj_t *volt_chart = NULL;
lv_obj_t *cur_chart = NULL;
lv_obj_t *fre = NULL;
lv_obj_t *duty = NULL;
lv_obj_t *pwm_btn = NULL;
lv_obj_t *uart_btn = NULL;
lv_obj_t *uart_list = NULL;
lv_obj_t *uart_extarea = NULL;
lv_obj_t *i2con = NULL;
lv_obj_t *i2c_extarea = NULL;
lv_obj_t *wireless_uart_btn = NULL;
lv_obj_t *wireless_uart_list = NULL;
lv_obj_t *wireless_uart_extarea = NULL;
lv_obj_t *DSO_chart = NULL;
lv_chart_series_t *DSO_ser = NULL;
lv_obj_t *dso_run_btn = NULL;
lv_obj_t *FRE_label = NULL;
lv_obj_t *freq_run_btn = NULL;

lv_timer_t *updatelabel_timer1 = NULL;
lv_timer_t *updatelabel_timer2 = NULL;
lv_timer_t *updatelabel_timer3 = NULL;
lv_timer_t *adddata_timer = NULL;
lv_timer_t *adddata_timer2 = NULL;
lv_timer_t *DSO_update_timer1 = NULL;
lv_timer_t *DSO_update_timer2 = NULL;
lv_timer_t *DSO_update_timer3 = NULL;
lv_timer_t *FRE_label_update_timer = NULL;
lv_timer_t *uart_rx_timer = NULL;
lv_timer_t *ble_rx_timer = NULL;
lv_timer_t *i2c_rx_timer = NULL;
lv_timer_t *pwm_sync_timer = NULL;
lv_timer_t *dso_poll_timer = NULL;

/* ------------------------------------------------------------------ */
/* 样式                                                                */
/* ------------------------------------------------------------------ */
static lv_style_t style_menu;  /* 菜单按键：红色圆角边框 */
static lv_style_t style_focus; /* 焦点：金色边框 */
static lv_style_t style_ctrl;  /* 页面内小按钮：黑底灰边 */

static bool s_styles_ready = false;

static void styles_init(void)
{
    if (s_styles_ready) return; /* 只初始化一次 */
    s_styles_ready = true;

    lv_style_init(&style_menu);
    lv_style_set_border_color(&style_menu, lv_color_hex(COL_RED));
    lv_style_set_border_width(&style_menu, 3);
    lv_style_set_bg_color(&style_menu, lv_color_hex(0x000000));
    lv_style_set_radius(&style_menu, 20);

    lv_style_init(&style_focus);
    lv_style_set_border_color(&style_focus, lv_color_hex(COL_GOLD));
    lv_style_set_border_width(&style_focus, 4);

    lv_style_init(&style_ctrl);
    lv_style_set_bg_color(&style_ctrl, lv_color_hex(0x000000));
    lv_style_set_border_color(&style_ctrl, lv_color_hex(COL_GRAY));
    lv_style_set_border_width(&style_ctrl, 2);
    lv_style_set_radius(&style_ctrl, 6);
}

/* ------------------------------------------------------------------ */
/* 菜单项                                                              */
/* ------------------------------------------------------------------ */
#define TILE_TIMERS 8

typedef struct
{
    lv_obj_t *chart;
    lv_chart_series_t *ser;
} chart_feed_t;

typedef struct
{
    lv_obj_t *btn;   /* 菜单按键（功能关闭时可见） */
    lv_obj_t *tile;  /* 功能页面（打开时可见）     */
    lv_obj_t *x_btn; /* 页面右下角的 ×             */
    lv_obj_t *lbl1;  /* 页面里由定时器刷新的标签   */
    lv_obj_t *lbl2;
    lv_obj_t *lbl3;
    lv_obj_t *lbl4;
    chart_feed_t feed1;
    chart_feed_t feed2;
    uint32_t dso_seq;
    lv_timer_t *timers[TILE_TIMERS];
    uint8_t ntimers;
    bool open;
} exlink_entry_t;

static exlink_entry_t s_ent[EXLINK_FN_COUNT];
static lv_obj_t *s_keyboard = NULL;
static lv_timer_t *s_visible_timer = NULL;
static int32_t s_content_h = -1;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          lv_color_t color, bool recolor)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    if (recolor) lv_label_set_recolor(l, true);
    return l;
}

/* 页面内的一行（横向排布，子对象用 flex 排列） */
static lv_obj_t *tile_row(lv_obj_t *parent, lv_coord_t h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, h);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_column(r, 3, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *tile_btn(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t h,
                          const lv_font_t *font, lv_color_t tcolor, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_add_style(b, &style_ctrl, 0);
    lv_obj_add_style(b, &style_focus, LV_STATE_FOCUSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = mk_label(b, txt, font, tcolor, false);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

/* 固定尺寸的文本框：lv_textarea_set_one_line() 会改尺寸策略（LV_SIZE_CONTENT），
 * 必须先调用它再设尺寸，否则文本框会随内容长高、页面越来越高 */
static lv_obj_t *tile_textarea(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                               const char *placeholder, bool one_line)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ta, 3, LV_PART_MAIN);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_add_event_cb(ta, textarea_click_event_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

/* 波特率下拉框（与原固件相同的档位） */
static lv_obj_t *tile_baud_dropdown(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                    lv_event_cb_t cb)
{
    static const char *const baud[] = {
        "1200", "2400", "4800", "9600", "19200", "43000", "76800",
        "115200", "128000", "230400", "256000", "460800", "921600"};
    uint32_t i;

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_size(dd, w, h);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dd, 2, LV_PART_MAIN);
    lv_dropdown_set_options(dd, "");
    for (i = 0; i < sizeof(baud) / sizeof(baud[0]); i++)
    {
        lv_dropdown_add_option(dd, baud[i], i);
    }
    lv_dropdown_set_selected(dd, 7); /* 115200 */
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *tile_chart(lv_obj_t *parent, lv_coord_t h, uint16_t points,
                            lv_coord_t ymin, lv_coord_t ymax)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CHARTBG), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_GRID), LV_PART_MAIN);
    lv_obj_set_style_line_color(c, lv_color_hex(COL_GRID), LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, points);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    lv_chart_set_div_line_count(c, 2, 4);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);  /* 只画线，不画点 */
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_chart_refresh(c);
    return c;
}

/* 小引脚方块（与原机 Pin Map 相同的配色/文字） */
static lv_obj_t *pin_box(lv_obj_t *parent, uint32_t color, const char *txt)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, 46, 18);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = mk_label(b, txt, &lv_font_montserrat_8, lv_color_hex(0xFFFFFF), false);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static void pin_strip(lv_obj_t *parent, const uint32_t *colors, const char *const *texts, int n)
{
    lv_obj_t *row = tile_row(parent, 20);
    int i;
    for (i = 0; i < n; i++)
    {
        pin_box(row, colors[i], texts[i]);
    }
    mk_label(row, "ROW1", &lv_font_montserrat_8, lv_color_hex(COL_GOLD), false);
}

/* 页面容器：宽跟随菜单列，高自适应内容；右下角有 × */
static lv_obj_t *tile_create(lv_obj_t *parent, int idx, const char *title, uint32_t head_color)
{
    exlink_entry_t *en = &s_ent[idx];

    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(t, 6, 0);
    lv_obj_set_style_pad_bottom(t, 30, 0); /* 给 × 留位置 */
    lv_obj_set_style_pad_row(t, 4, 0);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_border_width(t, 3, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILEBG), 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE); /* 滚动的是菜单列 */
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_style(t, &style_focus, LV_STATE_FOCUSED);

    mk_label(t, title, &lv_font_montserrat_14, lv_color_hex(head_color), false);

    lv_obj_t *x = tile_btn(t, LV_SYMBOL_CLOSE, 30, 22, &lv_font_montserrat_12,
                           lv_color_hex(0xFFFFFF), exlink_tile_close_event_cb);
    lv_obj_set_style_bg_color(x, lv_color_hex(0xC00000), 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT); /* 不参与 flex 排布 */
    lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, 2, 26); /* 贴到页面右下角 */
    lv_obj_set_user_data(x, (void *)(uintptr_t)(idx + 1));
    en->x_btn = x;

    /* 页面控件也要能处理触摸长按/右滑返回 */
    exlink_back_attach(t);
    return t;
}

/* 页面内部的容器/控件自己都可点击，会把触摸事件吃掉（页面就收不到右滑了），
 * 统一让它们把事件冒泡给页面：右滑/长按返回在页面任意位置都能生效 */
static void bubble_events_recursive(lv_obj_t *obj)
{
    uint32_t i, n = lv_obj_get_child_cnt(obj);
    for (i = 0; i < n; i++)
    {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        bubble_events_recursive(c);
    }
}

static void tile_finish(int idx)
{
    if (s_ent[idx].x_btn) lv_obj_move_foreground(s_ent[idx].x_btn);
    bubble_events_recursive(s_ent[idx].tile);
}

static lv_timer_t *tile_timer(int idx, lv_timer_cb_t cb, uint32_t period, void *user_data)
{
    exlink_entry_t *en = &s_ent[idx];
    lv_timer_t *t = lv_timer_create(cb, period, user_data);
    if (en->ntimers < TILE_TIMERS) en->timers[en->ntimers++] = t;
    return t;
}

static void tile_timers_clear(int idx)
{
    exlink_entry_t *en = &s_ent[idx];
    uint8_t i;
    for (i = 0; i < en->ntimers; i++)
    {
        if (en->timers[i]) lv_timer_del(en->timers[i]);
        en->timers[i] = NULL;
    }
    en->ntimers = 0;
}

/* ------------------------------------------------------------------ */
/* 焦点：始终让选中的菜单项完整可见                                     */
/* ------------------------------------------------------------------ */

static void keep_focus_visible(void)
{
    lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
    lv_area_t e, view;
    lv_coord_t sy;

    if (panel == NULL || focused == NULL) return;

    lv_obj_update_layout(lv_scr_act());
    lv_obj_get_coords(focused, &e);
    lv_obj_get_content_coords(panel, &view);

    sy = lv_obj_get_scroll_y(panel);

    if (e.y2 - e.y1 <= view.y2 - view.y1)
    {
        if (e.y1 < view.y1) sy -= (view.y1 - e.y1);
        else if (e.y2 > view.y2) sy += (e.y2 - view.y2);
    }
    else if (e.y1 != view.y1)
    {
        sy += (e.y1 - view.y1);
    }
    lv_obj_scroll_to_y(panel, sy, LV_ANIM_OFF);
}

/* 焦点一变（旋钮/Tab/方向键）就把选中的整页滚进屏幕 */
static void group_focus_cb(lv_group_t *g)
{
    lv_obj_t *focused = lv_group_get_focused(g);
    if (focused == NULL) return;

    /* 滚轮/旋钮离开正在编辑的页面时收起数字键盘 */
    if (s_keyboard && !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_t *ta = lv_keyboard_get_textarea(s_keyboard);
        if (ta && exlink_entry_of_obj(ta) != exlink_entry_of_obj(focused))
        {
            exlink_keyboard_hide();
        }
    }
    keep_focus_visible();
    exlink_dump_menu_geometry(); /* 串口自检：焦点一变就报告有没有滚到位 */
}

/* 页面运行时可能变高/变矮：内容高度一变就重新校正可见性；
 * 顺便每 200 ms 把"横向漂移"拉回 0 —— 点文本框时 LVGL 会调
 * lv_obj_scroll_to_view()，它是无边界滚动，会给本不该横向滚动的容器
 * （菜单列 / 屏幕）留下一个 x 偏移，表现就是整幅 UI 往左或往右偏，
 * 而且没人复位。菜单列本来就是纯竖直列表，x 必须恒为 0。 */
static void keep_focus_visible_cb(lv_timer_t *t)
{
    int32_t content;
    (void)t;
#if EXLINK_UI_DEBUG
    ui_shift_poll(); /* 串口微调界面水平位置 */
#endif
    if (panel == NULL) return;

    if (lv_obj_get_scroll_x(panel) != 0) lv_obj_scroll_to_x(panel, 0, LV_ANIM_OFF);

    if (lv_obj_get_scroll_x(lv_scr_act()) != 0 || lv_obj_get_scroll_y(lv_scr_act()) != 0)
    {
        lv_obj_scroll_to(lv_scr_act(), 0, 0, LV_ANIM_OFF); /* 屏幕永远不该滚 */
    }

    content = lv_obj_get_scroll_top(panel) + lv_obj_get_height(panel) +
              lv_obj_get_scroll_bottom(panel);
    if (content == s_content_h) return;
    s_content_h = content;
    keep_focus_visible();
}

/* ------------------------------------------------------------------ */
/* 搬运定时器：后台任务数据 -> 控件（全部在 LVGL 任务里跑）             */
/* ------------------------------------------------------------------ */

/* 文本区追加，超过上限就清空（长时间运行内存不增长；页面小，留 1200 字符足够） */
static void append_capped(lv_obj_t *ta, const char *txt)
{
    lv_textarea_add_text(ta, txt);
    if (strlen(lv_textarea_get_text(ta)) > 1200)
    {
        lv_textarea_set_text(ta, "");
    }
}

static void update_label_timer1(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), voltageStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void update_label_timer2(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), currentStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void update_label_timer3(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), powerStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void DSO_update_maxValue_timer(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), maxValueStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void DSO_update_minValue_timer(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), minValueStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void DSO_update_peakToPeakValue_timer(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), peakToPeakValueStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

static void FRE_label_update(lv_timer_t *timer)
{
    char buf[20];
    app_copy_str(buf, sizeof(buf), freqencyStr);
    lv_label_set_text((lv_obj_t *)timer->user_data, buf);
}

/* 电压/电流波形：把 INA226 的新值喂给图表 */
static void add_data(lv_timer_t *timer)
{
    chart_feed_t *f = (chart_feed_t *)timer->user_data;
    float v, a, w, mah;
    if (!f || !f->chart || !f->ser) return;
    app_get_ina(&v, &a, &w, &mah);
    lv_chart_set_next_value(f->chart, f->ser, (lv_coord_t)(v * 100)); /* 0.01 V */
}

static void add_data2(lv_timer_t *timer)
{
    chart_feed_t *f = (chart_feed_t *)timer->user_data;
    float v, a, w, mah;
    if (!f || !f->chart || !f->ser) return;
    app_get_ina(&v, &a, &w, &mah);
    lv_chart_set_next_value(f->chart, f->ser, (lv_coord_t)(a * 1000)); /* mA */
}

static void uart_rx_timer_cb(lv_timer_t *timer)
{
    char buf[129];
    size_t n;
    (void)timer;
    if (!uart_extarea) return;
    while ((n = app_uart_read(buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = 0;
        append_capped(uart_extarea, buf);
    }
}

static void ble_rx_timer_cb(lv_timer_t *timer)
{
    char buf[129];
    size_t n;
    (void)timer;
    if (!wireless_uart_extarea) return;
    while ((n = app_ble_read(buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = 0;
        append_capped(wireless_uart_extarea, buf);
    }
}

static void i2c_rx_timer_cb(lv_timer_t *timer)
{
    char buf[129];
    size_t n;
    (void)timer;
    if (!i2c_extarea) return;
    while ((n = app_i2c_read(buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = 0;
        append_capped(i2c_extarea, buf);
    }
    /* 扫描结束后按钮自动恢复红色 */
    if (i2con) lv_obj_set_style_bg_color(i2con, lv_color_hex(app_i2c_busy() ? COL_GREEN : COL_RED), 0);
}

/* DSO：把采样任务写进环形缓冲的新点搬进图表（序号随页面开关重置） */
static void dso_poll_timer_cb(lv_timer_t *timer)
{
    exlink_entry_t *en = &s_ent[EXLINK_FN_DSO];
    int buf[64];
    int n, i;
    (void)timer;
    if (!DSO_chart || !DSO_ser) return;
    n = app_dso_poll(&en->dso_seq, buf, 64);
    for (i = 0; i < n; i++)
    {
        lv_chart_set_next_value(DSO_chart, DSO_ser, (lv_coord_t)buf[i]);
    }
}

/* PWM：文本框改动后重新下发设定值（仅在输出打开时） */
static void pwm_sync_timer_cb(lv_timer_t *timer)
{
    int freq, d;
    (void)timer;
    if (!fre || !duty) return;
    if (!app_pwm_get(NULL, NULL)) return;
    freq = atoi(lv_textarea_get_text(fre));
    d = atoi(lv_textarea_get_text(duty));
    app_pwm_set(true, freq, d);
}

/* 电压表的波形 */
static void voltmeter_add_data(lv_timer_t *timer)
{
    add_data(timer);
}

/* 主菜单右列：后台正在运行的功能（RUN n + 名称），同时给菜单按键/页面着色 */
static void menu_status_timer_cb(lv_timer_t *timer)
{
    static const app_func_t map[EXLINK_FN_COUNT] = {
        FUNC_COUNT, FUNC_POWER, FUNC_PWM, FUNC_UART, FUNC_I2C,
        FUNC_COUNT, FUNC_DSO, FUNC_BLE, FUNC_FREQ, FUNC_COUNT};
    char names[64];
    int running = 0;
    int i;
    (void)timer;

    for (i = 0; i < EXLINK_FN_COUNT; i++)
    {
        bool on = (map[i] != FUNC_COUNT) && app_func_running(map[i]);
        uint32_t bg = on ? COL_RUNBG : 0x000000;
        if (on) running++;
        if (s_ent[i].open)
        {
            /* 页面开着：给页面底色，焦点边框不受影响 */
            if (s_ent[i].tile) lv_obj_set_style_bg_color(s_ent[i].tile, lv_color_hex(bg), 0);
        }
        else if (s_ent[i].btn)
        {
            lv_obj_set_style_bg_color(s_ent[i].btn, lv_color_hex(bg), 0);
        }
    }

    app_status_text(names, sizeof(names));
    if (status_detail) lv_label_set_text(status_detail, names);
    if (status_label) lv_label_set_text_fmt(status_label, "RUN %d", running);
}

/* 焦点指示滑条 + 电池图标 */
void update_slider_timer(lv_timer_t *timer)
{
    lv_obj_t *f = lv_group_get_focused(lv_group_get_default());
    int idx = exlink_entry_of_obj(f);
    int target = (idx >= 0) ? 10 - idx : 0;
    float bat;
    (void)timer;

    if (target && slider)
    {
        exlink_slider_set_syncing(true);
        lv_slider_set_value(slider, target, LV_ANIM_OFF);
        exlink_slider_set_syncing(false);
    }

    bat = app_get_battery();
    if (bat_label)
    {
        const char *sym = LV_SYMBOL_BATTERY_EMPTY;
        lv_coord_t x = 252;
        if (bat >= 3.95f) { sym = LV_SYMBOL_CHARGE; x = 258; }
        else if (bat >= 3.7f) sym = LV_SYMBOL_BATTERY_FULL;
        else if (bat >= 3.4f) sym = LV_SYMBOL_BATTERY_3;
        else if (bat >= 3.0f) sym = LV_SYMBOL_BATTERY_2;
        else if (bat >= 2.7f) sym = LV_SYMBOL_BATTERY_1;
        lv_obj_set_pos(bat_label, x, 200);
        lv_label_set_text(bat_label, sym);
    }
}

/* ------------------------------------------------------------------ */
/* 页面内容                                                            */
/* ------------------------------------------------------------------ */

static void build_pinmap(int idx, lv_obj_t *t)
{
    static const struct { uint32_t color; const char *abbr; const char *num; } pins[26] = {
        {0x696969, "GD", "1"},  {0xA52A2A, "IO", "2"},  {0xFF0000, "3V", "3"},
        {0xFF0000, "5V", "4"},  {0x008000, "CO", "5"},  {0x008000, "PW", "6"},
        {0x0000FF, "SL", "7"},  {0x0000FF, "SA", "8"},  {0xC71585, "DI", "9"},
        {0xC71585, "CL", "10"}, {0xC71585, "CL", "11"}, {0xC71585, "CL", "12"},
        {0xFF8C00, "TX", "13"}, {0x696969, "GD", "14"}, {0x20B2AA, "C0", "15"},
        {0x20B2AA, "C1", "16"}, {0x20B2AA, "C2", "17"}, {0x20B2AA, "C3", "18"},
        {0x20B2AA, "C4", "19"}, {0x20B2AA, "C5", "20"}, {0x20B2AA, "C6", "21"},
        {0x20B2AA, "C7", "22"}, {0x006400, "DI", "23"}, {0x006400, "CL", "24"},
        {0xFF1493, "RX", "25"}, {0xFF1493, "TX", "26"}};
    static const char *const row_title[2] = {
        "#FFD700 ROW1: MCU and power#",
        "#00FFFF ROW2: DLA and DAPlink#"};
    int r, i;
    (void)idx;

    for (r = 0; r < 2; r++)
    {
        mk_label(t, row_title[r], &lv_font_montserrat_8, lv_color_hex(0xFFFFFF), true);
        lv_obj_t *row = tile_row(t, 40);
        for (i = 0; i < 13; i++)
        {
            const int k = r * 13 + i;
            lv_obj_t *p = lv_obj_create(row);
            lv_obj_set_width(p, 1);
            lv_obj_set_height(p, 38);
            lv_obj_set_flex_grow(p, 1); /* 13 个引脚均分列宽 */
            lv_obj_set_style_radius(p, 4, 0);
            lv_obj_set_style_border_width(p, 0, 0);
            lv_obj_set_style_bg_color(p, lv_color_hex(pins[k].color), 0);
            lv_obj_set_style_pad_all(p, 0, 0);
            lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *top = mk_label(p, pins[k].abbr, &lv_font_montserrat_8,
                                     lv_color_hex(0xFFFFFF), false);
            lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 1);
            lv_obj_t *bot = mk_label(p, pins[k].num, &lv_font_montserrat_10,
                                     lv_color_hex(0xFFFFFF), false);
            lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, -1);
        }
    }
}

static void build_power(int idx, lv_obj_t *t)
{
    exlink_entry_t *en = &s_ent[idx];
    lv_obj_t *row;
    static const uint32_t pin_colors[2] = {0x696969, 0x8B0000};
    static const char *const pin_texts[2] = {"1 GND", "2 OUT"};

    /* Exlink3.1：MP28167 是"按电压设定"的，所以本页给一个设定值输入框
       （点一下弹数字键盘，和 PWM 页的 FRE/DUTY 一样；开输出时按输入框里的值设压） */
    row = tile_row(t, 24);
    mk_label(row, "SET", &lv_font_montserrat_10, lv_color_hex(COL_GOLD), false);
    volt_set = tile_textarea(row, 62, 24, "0-20V", true);
    mk_label(row, "V", &lv_font_montserrat_10, lv_color_hex(COL_GOLD), false);
    {
        char b[12];
        int mv = app_get_volt_mv();
        snprintf(b, sizeof(b), "%d.%d", mv / 1000, (mv % 1000) / 100);
        lv_textarea_set_text(volt_set, b);
    }

    row = tile_row(t, 20);
    en->lbl1 = mk_label(row, "0.000", &lv_font_montserrat_14, lv_color_hex(COL_RED), false);
    lv_obj_set_width(en->lbl1, 46);
    lv_obj_set_style_text_align(en->lbl1, LV_TEXT_ALIGN_CENTER, 0);
    mk_label(row, "V", &lv_font_montserrat_10, lv_color_hex(COL_RED), false);
    en->lbl2 = mk_label(row, "0.000", &lv_font_montserrat_14, lv_color_hex(COL_GREEN), false);
    lv_obj_set_width(en->lbl2, 46);
    lv_obj_set_style_text_align(en->lbl2, LV_TEXT_ALIGN_CENTER, 0);
    mk_label(row, "A", &lv_font_montserrat_10, lv_color_hex(COL_GREEN), false);
    en->lbl3 = mk_label(row, "0.000", &lv_font_montserrat_14, lv_color_hex(COL_CYAN), false);
    lv_obj_set_width(en->lbl3, 46);
    lv_obj_set_style_text_align(en->lbl3, LV_TEXT_ALIGN_CENTER, 0);
    mk_label(row, "W", &lv_font_montserrat_10, lv_color_hex(COL_CYAN), false);

    volt_chart = tile_chart(t, 26, 15, 0, 1300); /* 0.01 V -> 0..13 V */
    en->feed1.chart = volt_chart;
    en->feed1.ser = lv_chart_add_series(volt_chart, lv_color_hex(COL_RED), LV_CHART_AXIS_PRIMARY_Y);
    cur_chart = tile_chart(t, 26, 15, 0, 5000); /* mA -> 0..5 A */
    en->feed2.chart = cur_chart;
    en->feed2.ser = lv_chart_add_series(cur_chart, lv_color_hex(COL_GREEN), LV_CHART_AXIS_PRIMARY_Y);

    row = tile_row(t, 26);
    lv_obj_t *poweron = tile_btn(row, LV_SYMBOL_POWER, 30, 24, &lv_font_montserrat_14,
                                 lv_color_hex(COL_RED), poweronbtn_event_cb);
    lv_obj_set_style_bg_color(poweron, lv_color_hex(app_get_output() ? COL_GREEN : COL_RED), 0);
    poweron_label = lv_obj_get_child(poweron, 0);
    lv_obj_set_style_text_color(poweron_label, lv_color_hex(app_get_output() ? COL_GREEN : COL_RED), 0);
    tile_btn(row, LV_SYMBOL_PLUS, 24, 24, &lv_font_montserrat_12, lv_color_hex(0x87CEEB), VUPbtn_event_cb);
    tile_btn(row, LV_SYMBOL_MINUS, 24, 24, &lv_font_montserrat_12, lv_color_hex(0x87CEEB), VDOWNbtn_event_cb);
    tile_btn(row, "11V", 30, 24, &lv_font_montserrat_10, lv_color_hex(COL_GOLD), V11btn_event_cb);
    tile_btn(row, "5V", 30, 24, &lv_font_montserrat_10, lv_color_hex(COL_GOLD), V5btn_event_cb);
    tile_btn(row, "3V", 30, 24, &lv_font_montserrat_10, lv_color_hex(COL_GOLD), V3btn_event_cb);

    pin_strip(t, pin_colors, pin_texts, 2);
}

static void build_pwm(int idx, lv_obj_t *t)
{
    static const uint32_t pin_colors[2] = {0x696969, 0x8B0000};
    static const char *const pin_texts[2] = {"1 GND", "6 PWM"};
    lv_obj_t *row;
    (void)idx;

    row = tile_row(t, 26);
    mk_label(row, "FRE", &lv_font_montserrat_10, lv_color_hex(COL_GOLD), false);
    fre = tile_textarea(row, 74, 24, "0-100K", true);
    mk_label(row, "Hz", &lv_font_montserrat_10, lv_color_hex(COL_GOLD), false);
    pwm_btn = tile_btn(row, "OPEN", 52, 24, &lv_font_montserrat_12,
                       lv_color_hex(0xFFFFFF), pwm_btn_event_cb);
    lv_obj_set_style_bg_color(pwm_btn, lv_color_hex(app_pwm_get(NULL, NULL) ? COL_GREEN : COL_RED), 0);

    row = tile_row(t, 26);
    mk_label(row, "DUTY", &lv_font_montserrat_10, lv_color_hex(0x87CEFA), false);
    duty = tile_textarea(row, 74, 24, "0-100", true);
    mk_label(row, "%", &lv_font_montserrat_10, lv_color_hex(0x87CEFA), false);

    /* 原机的 PWM 引脚图，缩到 3/8 显示 */
    row = tile_row(t, 34);
    lv_obj_t *img = lv_img_create(row);
    lv_img_set_src(img, &pwmint_png);
    lv_img_set_zoom(img, 96); /* 256 = 100% */

    pin_strip(t, pin_colors, pin_texts, 2);
}

static void build_uart(int idx, lv_obj_t *t)
{
    lv_obj_t *row;
    (void)idx;
    row = tile_row(t, 28);
    uart_btn = tile_btn(row, "OPEN", 52, 26, &lv_font_montserrat_12,
                        lv_color_hex(0xFFFFFF), uart_btn_event_cb);
    lv_obj_set_style_bg_color(uart_btn, lv_color_hex(app_uart_on() ? COL_GREEN : COL_RED), 0);
    uart_list = tile_baud_dropdown(row, 92, 26, uart_list_event_cb);
    mk_label(row, "baud", &lv_font_montserrat_8, lv_color_hex(COL_CYAN), false);

    uart_extarea = tile_textarea(t, LV_PCT(100), 46, "RX data", false);
    lv_obj_set_style_text_font(uart_extarea, &lv_font_montserrat_8, LV_PART_MAIN);
}

static void build_i2c(int idx, lv_obj_t *t)
{
    lv_obj_t *row;
    (void)idx;
    row = tile_row(t, 28);
    i2con = tile_btn(row, "SCAN", 58, 26, &lv_font_montserrat_12,
                     lv_color_hex(0xFFFFFF), i2conbtn_event_cb);
    lv_obj_set_style_bg_color(i2con, lv_color_hex(app_i2c_busy() ? COL_GREEN : COL_RED), 0);
    mk_label(row, "I2C bus 0x03-0x77", &lv_font_montserrat_8, lv_color_hex(COL_CYAN), false);

    i2c_extarea = tile_textarea(t, LV_PCT(100), 46, "scan result", false);
    lv_obj_set_style_text_font(i2c_extarea, &lv_font_montserrat_8, LV_PART_MAIN);
}

static void build_voltmeter(int idx, lv_obj_t *t)
{
    exlink_entry_t *en = &s_ent[idx];
    lv_obj_t *row = tile_row(t, 24);

    en->lbl1 = mk_label(row, "0.000", &lv_font_montserrat_20, lv_color_hex(COL_RED), false);
    mk_label(row, "V", &lv_font_montserrat_10, lv_color_hex(COL_RED), false);

    volt_chart = tile_chart(t, 56, 15, 0, 1300);
    en->feed1.chart = volt_chart;
    en->feed1.ser = lv_chart_add_series(volt_chart, lv_color_hex(COL_RED), LV_CHART_AXIS_PRIMARY_Y);
}

static void build_dso(int idx, lv_obj_t *t)
{
    exlink_entry_t *en = &s_ent[idx];
    static const uint32_t pin_colors[2] = {0x696969, 0xFF8C00};
    static const char *const pin_texts[2] = {"1 GND", "5 IN"};
    lv_obj_t *row = tile_row(t, 13);

    mk_label(row, "max", &lv_font_montserrat_8, lv_color_hex(COL_RED), false);
    en->lbl1 = mk_label(row, "0.00", &lv_font_montserrat_8, lv_color_hex(COL_RED), false);
    mk_label(row, "min", &lv_font_montserrat_8, lv_color_hex(COL_GREEN), false);
    en->lbl2 = mk_label(row, "0.00", &lv_font_montserrat_8, lv_color_hex(COL_GREEN), false);
    mk_label(row, "vpp", &lv_font_montserrat_8, lv_color_hex(COL_CYAN), false);
    en->lbl3 = mk_label(row, "0.00", &lv_font_montserrat_8, lv_color_hex(COL_CYAN), false);

    DSO_chart = tile_chart(t, 70, 64, 0, 4095);
    lv_chart_set_div_line_count(DSO_chart, 7, 11);
    DSO_ser = lv_chart_add_series(DSO_chart, lv_color_hex(0xFFFF00), LV_CHART_AXIS_PRIMARY_Y);

    row = tile_row(t, 26);
    /* 原机的 RUN/STOP：STOP 之后采样任务阻塞，不再占 CPU */
    dso_run_btn = tile_btn(row, app_dso_on() ? "RUN" : "STOP", 54, 24, &lv_font_montserrat_12,
                           lv_color_hex(0xFFFFFF), dso_run_btn_event_cb);
    lv_obj_set_style_bg_color(dso_run_btn, lv_color_hex(app_dso_on() ? COL_GREEN : COL_RED), 0);
    pin_strip(t, pin_colors, pin_texts, 2);
}

static void build_ble(int idx, lv_obj_t *t)
{
    lv_obj_t *row;
    (void)idx;
    row = tile_row(t, 28);
    mk_label(row, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_14, lv_color_hex(COL_BLUE), false);
    wireless_uart_btn = tile_btn(row, "OPEN", 52, 26, &lv_font_montserrat_12,
                                 lv_color_hex(0xFFFFFF), wireless_uart_btn_event_cb);
    lv_obj_set_style_bg_color(wireless_uart_btn, lv_color_hex(app_ble_on() ? COL_GREEN : COL_RED), 0);
    wireless_uart_list = tile_baud_dropdown(row, 92, 26, wireless_uart_list_event_cb);

    wireless_uart_extarea = tile_textarea(t, LV_PCT(100), 46, "BLE RX data", false);
    lv_obj_set_style_text_font(wireless_uart_extarea, &lv_font_montserrat_8, LV_PART_MAIN);
}

static void build_fre(int idx, lv_obj_t *t)
{
    exlink_entry_t *en = &s_ent[idx];
    lv_obj_t *row = tile_row(t, 34);

    en->lbl1 = mk_label(row, "0", &lv_font_montserrat_32, lv_color_hex(COL_CYAN), false);
    mk_label(row, "Hz", &lv_font_montserrat_12, lv_color_hex(COL_RED), false);

    row = tile_row(t, 26);
    freq_run_btn = tile_btn(row, app_freq_on() ? "RUN" : "STOP", 54, 24, &lv_font_montserrat_12,
                            lv_color_hex(0xFFFFFF), freq_run_btn_event_cb);
    lv_obj_set_style_bg_color(freq_run_btn, lv_color_hex(app_freq_on() ? COL_GREEN : COL_RED), 0);
}

static void build_info(int idx, lv_obj_t *t)
{
    static const char *const lines[] = {
        "Exlink3.0  ESP32-S3  multitool",
        "menu key -> function page, several",
        "functions keep running at once",
        "knob: move / press: enter",
        "hold: close the page (x)",
        "original UI model: EXlink2.1"};
    unsigned i;
    lv_obj_t *img;
    (void)idx;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    {
        mk_label(t, lines[i], &lv_font_montserrat_8, lv_color_hex(0xCCCCCC), false);
    }
    img = lv_img_create(t);
    lv_img_set_src(img, &Exlink_png);
    lv_img_set_zoom(img, 128); /* 210x33 -> 105x17 */
}

/* ------------------------------------------------------------------ */
/* 菜单项定义表                                                        */
/* ------------------------------------------------------------------ */
typedef struct
{
    const char *title;
    const void *icon;
    uint8_t btn_font;
    uint32_t head_color;
    void (*build)(int idx, lv_obj_t *tile);
} exlink_func_t;

static const exlink_func_t s_funcs[EXLINK_FN_COUNT] = {
    {"Pin Map", &pinmap_png, 20, COL_GOLD, build_pinmap},
    {"DC POWER", &power_png, 20, COL_RED, build_power},
    {"PWM OUT", &pwm_png, 20, COL_GOLD, build_pwm},
    {"UART HELPER", &usarthelper_png, 16, COL_GREEN, build_uart},
    {"I2C SCAN", &i2c_png, 20, COL_CYAN, build_i2c},
    {"Voltmeter", &voltmeter_png, 20, COL_RED, build_voltmeter},
    {"Simple DSO", &DSO_png, 20, COL_CYAN, build_dso},
    {"BLE UART", &wireless_png, 20, COL_BLUE, build_ble},
    {"FRE Count", &FREcounter_png, 20, COL_CYAN, build_fre},
    {"Device INFO", &readme_png, 20, COL_GOLD, build_info}};

/* ------------------------------------------------------------------ */
/* 功能页打开 = 界面侧搬运定时器 + 后端"这一页开着"登记                 */
/* ------------------------------------------------------------------ */

static void func_activate(int idx)
{
    exlink_entry_t *en = &s_ent[idx];
    app_page_t page = APP_PAGE_HOME;

    switch (idx)
    {
    case EXLINK_FN_POWER:
        page = APP_PAGE_POWER;
        tile_timer(idx, update_label_timer1, 100, en->lbl1);
        tile_timer(idx, update_label_timer2, 100, en->lbl2);
        tile_timer(idx, update_label_timer3, 100, en->lbl3);
        tile_timer(idx, add_data, 100, &en->feed1);
        tile_timer(idx, add_data2, 100, &en->feed2);
        break;

    case EXLINK_FN_VOLTMETER:
        page = APP_PAGE_VOLTMETER;
        tile_timer(idx, update_label_timer1, 100, en->lbl1);
        tile_timer(idx, voltmeter_add_data, 100, &en->feed1);
        break;

    case EXLINK_FN_PWM:
        page = APP_PAGE_PWM;
        /* 文本框预置当前设定值，OPEN 时就有合理的频率/占空比 */
        {
            int f = 0, d = 0;
            char b[16];
            app_pwm_get(&f, &d);
            if (f <= 0) f = 1000;
            if (d <= 0) d = 50;
            snprintf(b, sizeof(b), "%d", f);
            if (lv_textarea_get_text(fre)[0] == 0) lv_textarea_set_text(fre, b);
            snprintf(b, sizeof(b), "%d", d);
            if (lv_textarea_get_text(duty)[0] == 0) lv_textarea_set_text(duty, b);
        }
        tile_timer(idx, pwm_sync_timer_cb, 300, NULL);
        break;

    case EXLINK_FN_UART:
        page = APP_PAGE_UART;
        tile_timer(idx, uart_rx_timer_cb, 100, NULL);
        break;

    case EXLINK_FN_I2C:
        page = APP_PAGE_I2C;
        tile_timer(idx, i2c_rx_timer_cb, 100, NULL);
        break;

    case EXLINK_FN_DSO:
        page = APP_PAGE_DSO;
        en->dso_seq = 0;
        tile_timer(idx, DSO_update_maxValue_timer, 50, en->lbl1);
        tile_timer(idx, DSO_update_minValue_timer, 50, en->lbl2);
        tile_timer(idx, DSO_update_peakToPeakValue_timer, 50, en->lbl3);
        tile_timer(idx, dso_poll_timer_cb, 30, NULL);
        app_dso_set(true); /* 与原固件一致：打开页面即开始采样（可再按 STOP） */
        break;

    case EXLINK_FN_BLE:
        page = APP_PAGE_BLE;
        tile_timer(idx, ble_rx_timer_cb, 100, NULL);
        break;

    case EXLINK_FN_FRE:
        page = APP_PAGE_FREQ;
        tile_timer(idx, FRE_label_update, 200, en->lbl1);
        app_freq_set(true); /* 与原固件一致：打开页面即开始计数（可再按 STOP） */
        break;

    case EXLINK_FN_INFO:
        page = APP_PAGE_INFO;
        break;

    default: /* EXLINK_FN_PINMAP：没有后端需求，只是登记页面 */
        page = APP_PAGE_PINMAP;
        break;
    }

    app_ui_page_open(page); /* 后端据此判断"有没有人看"（INA226 采样档位） */
}

/* 页面关闭：删掉搬运定时器（后台任务继续跑），登记页面关闭 */
static void func_deactivate(int idx)
{
    app_page_t page = APP_PAGE_HOME;

    tile_timers_clear(idx);

    switch (idx)
    {
    case EXLINK_FN_POWER:     page = APP_PAGE_POWER; break;
    case EXLINK_FN_VOLTMETER: page = APP_PAGE_VOLTMETER; break;
    case EXLINK_FN_PWM:       page = APP_PAGE_PWM; break;
    case EXLINK_FN_UART:      page = APP_PAGE_UART; break;
    case EXLINK_FN_I2C:       page = APP_PAGE_I2C; break;
    case EXLINK_FN_DSO:       page = APP_PAGE_DSO; break;
    case EXLINK_FN_BLE:       page = APP_PAGE_BLE; break;
    case EXLINK_FN_FRE:       page = APP_PAGE_FREQ; break;
    case EXLINK_FN_INFO:      page = APP_PAGE_INFO; break;
    default:                  page = APP_PAGE_PINMAP; break;
    }

    app_ui_page_close(page);

    /* 离开 DC POWER 不改输出状态：Exlink3.0 的功能在后台继续跑 */
}

/* ------------------------------------------------------------------ */
/* 打开 / 关闭功能页面                                                 */
/* ------------------------------------------------------------------ */

bool exlink_function_is_open(int idx)
{
    if (idx < 0 || idx >= EXLINK_FN_COUNT) return false;
    return s_ent[idx].open;
}

int exlink_open_function_count(void)
{
    int n = 0, i;
    for (i = 0; i < EXLINK_FN_COUNT; i++)
    {
        if (s_ent[i].open) n++;
    }
    return n;
}

lv_obj_t *exlink_function_tile(int idx)
{
    if (idx < 0 || idx >= EXLINK_FN_COUNT) return NULL;
    return s_ent[idx].tile;
}

lv_obj_t *exlink_entry_focus_obj(int idx)
{
    if (idx < 0 || idx >= EXLINK_FN_COUNT) return NULL;
    return s_ent[idx].open ? s_ent[idx].tile : s_ent[idx].btn;
}

int exlink_entry_of_obj(lv_obj_t *obj)
{
    int i;
    for (; obj; obj = lv_obj_get_parent(obj))
    {
        for (i = 0; i < EXLINK_FN_COUNT; i++)
        {
            if (obj == s_ent[i].btn || obj == s_ent[i].tile) return i;
        }
    }
    return -1;
}

void exlink_open_function(int idx)
{
    exlink_entry_t *en;
    if (idx < 0 || idx >= EXLINK_FN_COUNT) return;
    en = &s_ent[idx];
    if (en->btn == NULL || en->tile == NULL) return;

    if (!en->open)
    {
        en->open = true;
        func_activate(idx); /* 启动这一页的搬运定时器 + 后端登记 */

        lv_obj_add_flag(en->btn, LV_OBJ_FLAG_HIDDEN);     /* 按键 -> 页面 */
        lv_obj_clear_flag(en->tile, LV_OBJ_FLAG_HIDDEN);
    }

    /* 焦点选中"整页"（页面内部控件不进焦点组） */
    lv_group_focus_obj(en->tile);
    keep_focus_visible();

    /* 板子上可直接从串口核对：同屏页面数 + 剩余堆（10 个页面常驻，堆要给够） */
    app_debug_mem("page open", idx);
}

void exlink_close_function(int idx)
{
    exlink_entry_t *en;
    if (idx < 0 || idx >= EXLINK_FN_COUNT) return;
    en = &s_ent[idx];
    if (!en->open || en->tile == NULL || en->btn == NULL) return;

    en->open = false;
    func_deactivate(idx); /* 停掉这一页的搬运定时器 */

    lv_obj_add_flag(en->tile, LV_OBJ_FLAG_HIDDEN); /* 页面 -> 按键 */
    lv_obj_clear_flag(en->btn, LV_OBJ_FLAG_HIDDEN);

    lv_group_focus_obj(en->btn);
    keep_focus_visible();

    app_debug_mem("page closed", idx);
}

void exlink_close_all_functions(void)
{
    int i;
    for (i = 0; i < EXLINK_FN_COUNT; i++)
    {
        exlink_close_function(i);
    }
}

void exlink_close_focused_function(void)
{
    int idx = exlink_entry_of_obj(lv_group_get_focused(lv_group_get_default()));
    if (idx >= 0 && exlink_function_is_open(idx))
    {
        exlink_close_function(idx);
    }
}

/* ------------------------------------------------------------------ */
/* 串口自检：菜单列几何 / 可滚动余量 / 焦点是否完整可见                 */
/* ------------------------------------------------------------------ */
void exlink_dump_menu_geometry(void)
{
    lv_area_t pa, view, fa;
    lv_obj_t *f;
    int idx;

    if (!EXLINK_UI_DEBUG) return;
    if (panel == NULL)
    {
        exlink_log("[geom] panel=NULL\n");
        return;
    }

    lv_obj_update_layout(lv_scr_act());
    lv_obj_get_coords(panel, &pa);
    lv_obj_get_content_coords(panel, &view);

    /* 一行打印：bottom>0 才说明菜单列真的有内容可以滚；
       焦点项不在 view 里（OFF-SCREEN）说明 keep_focus_visible 没起作用 */
    f = lv_group_get_focused(lv_group_get_default());
    if (f == NULL)
    {
        exlink_log("[geom] focus=NULL scrollY=%d bottom=%d\n",
                   (int)lv_obj_get_scroll_y(panel), (int)lv_obj_get_scroll_bottom(panel));
        return;
    }
    idx = exlink_entry_of_obj(f);
    lv_obj_get_coords(f, &fa);
    exlink_log("[geom] idx=%d %s y=%d..%d %s | scrollY=%d top=%d bottom=%d\n", idx,
               (idx >= 0 && f == s_ent[idx].tile) ? "page" : "btn",
               (int)fa.y1, (int)fa.y2,
               (fa.y1 >= view.y1 && fa.y2 <= view.y2) ? "on-screen" : "OFF-SCREEN",
               (int)lv_obj_get_scroll_y(panel),
               (int)lv_obj_get_scroll_top(panel),
               (int)lv_obj_get_scroll_bottom(panel));
}

/* ------------------------------------------------------------------ */
/* 数字键盘浮层                                                        */
/* ------------------------------------------------------------------ */

void exlink_keyboard_show(lv_obj_t *textarea)
{
    if (s_keyboard == NULL || textarea == NULL) return;
    lv_keyboard_set_textarea(s_keyboard, textarea);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

void exlink_keyboard_hide(void)
{
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

bool exlink_keyboard_visible(void)
{
    return s_keyboard != NULL && !lv_obj_has_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ */
/* 开机动画                                                            */
/* ------------------------------------------------------------------ */

void create_boot_animation(void)
{
    lv_obj_t *logo;
    lv_obj_t *line;
    lv_obj_t *title;
    static lv_point_t line_pts[] = {{0, 0}, {0, 70}};
    lv_anim_t a1, a2, a3, a4;
    lv_anim_timeline_t *tl;

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

    ui_root_prepare(); /* 整幅 UI（含开机动画）挂在根容器上，见 UI_X_SHIFT */
    lv_obj_clean(ui_root);

    logo = lv_img_create(ui_root);
    lv_img_set_src(logo, &ui_img_game3_png);
    lv_obj_set_pos(logo, 35, 50);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);

    line = lv_line_create(ui_root);
    lv_line_set_points(line, line_pts, 2);
    lv_obj_set_pos(line, 60, -100);
    lv_obj_set_style_line_width(line, 10, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);

    title = lv_img_create(ui_root);
    lv_img_set_src(title, &Exlink_png);
    lv_obj_set_pos(title, 55, 240);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_init(&a1);
    lv_anim_set_var(&a1, logo);
    lv_anim_set_exec_cb(&a1, anim_cb1);
    lv_anim_set_values(&a1, 35, 80);
    lv_anim_set_time(&a1, 450);
    lv_anim_set_path_cb(&a1, lv_anim_path_overshoot);

    lv_anim_init(&a2);
    lv_anim_set_var(&a2, line);
    lv_anim_set_exec_cb(&a2, anim_cb2);
    lv_anim_set_values(&a2, -100, 15);
    lv_anim_set_time(&a2, 450);
    lv_anim_set_path_cb(&a2, lv_anim_path_overshoot);

    lv_anim_init(&a3);
    lv_anim_set_var(&a3, title);
    lv_anim_set_exec_cb(&a3, anim_cb2);
    lv_anim_set_values(&a3, 240, 150);
    lv_anim_set_time(&a3, 450);
    lv_anim_set_path_cb(&a3, lv_anim_path_overshoot);

    /* 停 2 s 再进主菜单 */
    lv_anim_init(&a4);
    lv_anim_set_var(&a4, title);
    lv_anim_set_exec_cb(&a4, anim_cb1);
    lv_anim_set_values(&a4, 55, 55);
    lv_anim_set_time(&a4, 2000);
    lv_anim_set_ready_cb(&a4, anim_end_callback);

    tl = lv_anim_timeline_create();
    lv_anim_timeline_add(tl, 0, &a1);
    lv_anim_timeline_add(tl, 150, &a2);
    lv_anim_timeline_add(tl, 200, &a3);
    lv_anim_timeline_add(tl, 200, &a4);
    lv_anim_timeline_start(tl);
}

/* ------------------------------------------------------------------ */
/* 主菜单                                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *menu_btn(lv_obj_t *parent, const char *txt, const void *icon_src,
                          int fsize, int idx)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, 68);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_style(b, &style_menu, 0);
    lv_obj_add_style(b, &style_focus, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(b, menu_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(b, (void *)(uintptr_t)(idx + 1));
    exlink_back_attach(b);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_text_font(l, fsize == 16 ? &lv_font_montserrat_16 : &lv_font_montserrat_20, 0);

    lv_obj_t *ic = lv_img_create(b);
    lv_img_set_src(ic, icon_src);
    lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -10, 0);
    return b;
}

void exlink_show_menu(void)
{
    lv_obj_t *hint;
    lv_group_t *g;
    int i;

    exlink_close_all_functions(); /* 先停掉旧菜单里所有页面的定时器 */
    if (slider_update_timer) { lv_timer_del(slider_update_timer); slider_update_timer = NULL; }
    if (menu_status_timer) { lv_timer_del(menu_status_timer); menu_status_timer = NULL; }

    lv_obj_clean(ui_root_prepare());
    /* 屏幕本身永远不滚动：整幅 UI 的位移只允许通过菜单列（panel）的竖直滚动发生 */
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    s_keyboard = NULL;
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

    /* 左列：菜单按键与功能页面在同一个 flex 流里，页面就出现在原按键的位置 */
    panel = lv_obj_create(ui_root);
    lv_obj_set_size(panel, 248, 240);
    lv_obj_set_pos(panel, 2, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    exlink_back_attach(panel); /* 菜单列的空隙处也能右滑返回 */

    for (i = 0; i < EXLINK_FN_COUNT; i++)
    {
        exlink_entry_t *en = &s_ent[i];
        en->open = false;
        en->ntimers = 0;
        en->dso_seq = 0;

        en->btn = menu_btn(panel, s_funcs[i].title, s_funcs[i].icon, s_funcs[i].btn_font, i);
        en->tile = tile_create(panel, i, s_funcs[i].title, s_funcs[i].head_color);
        s_funcs[i].build(i, en->tile);
        tile_finish(i);
        lv_obj_add_flag(en->tile, LV_OBJ_FLAG_HIDDEN);
    }

    /* 右列：焦点滑条 / RUN 状态 / 电池 */
    slider = lv_slider_create(ui_root);
    lv_obj_set_size(slider, 14, 78);
    lv_obj_set_pos(slider, 256, 20);
    lv_slider_set_range(slider, 1, 10);
    lv_slider_set_value(slider, 10, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_RED), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), LV_PART_INDICATOR);
    /* 只订阅"值变了"这一种事件（EXlink2.2 就是这么接的）。
       LV_EVENT_ALL 会把重绘、命中测试、样式变化等内部事件也送进来，
       每来一次都会重新 focus 一次 → keep_focus_visible() 把菜单列拉回焦点项，
       用户的滑动/旋钮刚滚过去就被拉回来，于是"滑不动"。 */
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    status_label = lv_label_create(ui_root);
    lv_label_set_text(status_label, "RUN 0");
    lv_obj_set_style_text_color(status_label, lv_color_hex(COL_GOLD), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(status_label, 252, 104);

    status_detail = lv_label_create(ui_root);
    /* 整幅右移 UI_X_SHIFT 之后，这两行 8 号小字会顶到屏幕右边缘被裁掉，
       所以宽度一起收窄（UI_X_SHIFT=0 时就是原来的 66） */
    lv_obj_set_width(status_detail, 66 - UI_X_SHIFT);
    lv_label_set_long_mode(status_detail, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status_detail, "none");
    lv_obj_set_style_text_color(status_detail, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(status_detail, &lv_font_montserrat_8, 0);
    lv_obj_set_pos(status_detail, 252, 120);

    hint = mk_label(ui_root, "knob  move\npress enter\nhold  close page",
                    &lv_font_montserrat_8, lv_color_hex(COL_GRAY), false);
    lv_obj_set_width(hint, 66 - UI_X_SHIFT);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, 252, 158);

    bat_label = lv_label_create(ui_root);
    lv_label_set_text(bat_label, " ");
    lv_obj_set_pos(bat_label, 252, 200);
    lv_obj_set_style_text_color(bat_label, lv_color_hex(0x32CD32), 0);
    lv_obj_set_style_text_font(bat_label, &lv_font_montserrat_24, 0);

    /* 数字键盘（屏幕级浮层，不会被窄页面裁剪） */
    s_keyboard = lv_keyboard_create(ui_root);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
    /* 键盘也跟着整幅右移，宽度收掉同样的像素，避免右边的 3/6/9/退格 被屏幕边缘裁掉 */
    lv_obj_set_size(s_keyboard, 320 - UI_X_SHIFT, 110);
    lv_obj_set_pos(s_keyboard, 0, 130);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_handler, LV_EVENT_VALUE_CHANGED, NULL);

    exlink_back_attach(lv_scr_act());

    /* 焦点单位：功能没开 = 菜单按键；开了 = 整个页面（隐藏的那个自动跳过） */
    g = lv_group_create();
    lv_indev_set_group(indev_keypad, g);
    for (i = 0; i < EXLINK_FN_COUNT; i++)
    {
        lv_group_add_obj(g, s_ent[i].btn);
        lv_group_add_obj(g, s_ent[i].tile);
    }
    lv_group_set_focus_cb(g, group_focus_cb);
    lv_group_focus_obj(s_ent[0].btn);
    lv_group_set_default(g);

    slider_update_timer = lv_timer_create(update_slider_timer, 300, NULL);
    menu_status_timer = lv_timer_create(menu_status_timer_cb, 300, NULL);

    s_content_h = -1;
    if (s_visible_timer) lv_timer_del(s_visible_timer);
    s_visible_timer = lv_timer_create(keep_focus_visible_cb, 200, NULL);

    app_debug_mem("menu built", EXLINK_FN_COUNT);
    exlink_dump_menu_geometry(); /* 开机自检：菜单列到底能不能滚 */
}

void ui_Screen1_screen_init(void)
{
    exlink_show_menu();
}

void ui_init(void)
{
    lv_theme_default_init(NULL, lv_color_hex(0x000000), lv_color_hex(COL_RED),
                          LV_THEME_DEFAULT_DARK, LV_FONT_DEFAULT);
    styles_init();
    create_boot_animation();
}
