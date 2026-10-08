/**
 * @file  main.cpp
 * @brief Exlink3.0 —— 一机多用的掌上多功能仪器：FreeRTOS 多任务架构
 *
 * 设计目标：把多台台式仪器的功能集成到一台手持设备，并让它们同时工作——
 *           例如一边输出 PWM、一边收串口/BLE、一边测频率、一边监测电流电压，
 *           使用流程不被打断，工作台更简洁。
 *
 * 实现：每个功能一个独立任务（见 src/app/task_*.cpp），互不干扰、可并行运行；
 *       界面只负责显示与开关，切到别的功能页后台照常工作。
 *
 * 按需运行：任务不再靠"周期性 continue 轮询"活着——前端没调用（功能没开、界面也没用到）
 *       时，任务阻塞在 ulTaskNotifyTake(portMAX_DELAY) 上，CPU 占用为 0；
 *       界面开关（app_ui_page_open / app_ui_page_close）与功能开关（app_*_set）都会立刻把它唤醒。
 *
 * 任务分配：
 *   core 1 : lvgl     —— LVGL 界面（唯一允许调用 LVGL 的任务，5 ms 节拍，必须常驻）
 *   core 0 : ina226   —— INA226 功率/电池（功率或电压表界面、输出打开时 100 ms；
 *                                        主菜单只要电池图标时 1 s；否则阻塞）
 *            power    —— DC 输出开关 + MP28167 数控电源（I2C 写参考电压，纯事件触发）
 *            pwm      —— PWM 输出（GPIO5 / LEDC ch2，硬件输出，任务纯事件触发）
 *            dso      —— 简易示波器 ADC 采样（GPIO4，RUN 时 1 kHz，STOP 阻塞）
 *            freq     —— 频率计（PCNT 单元0，GPIO5，RUN 时 125 ms，STOP 暂停计数并阻塞）
 *            i2cscan  —— I2C 总线扫描（有请求才醒，首次扫描才占用引脚）
 *            uart     —— 串口助手收发（打开时 20 ms，关掉即阻塞）
 *            ble      —— BLE 无线串口（打开时 200 ms 巡检，关掉即阻塞且不初始化协议栈）
 */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <Wire.h>
#include "CST816T.h"
#include <WiFi.h>
#include "ui.h"
#include "app/app_shared.h"
#include "app/mp28167.h" /* Exlink3.1：MP28167 数控电源（替代 MCP4017 数字电位器） */
#include "stdio.h"
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <stdarg.h>

#define I2C_SDA 18 // 触摸SDA引脚
#define I2C_SCL 21 // 触摸SCL引脚
#define RST_N_PIN 16
#define INT_N_PIN -1
#define RIGHT_PIN 38
#define LEFT_PIN 39
#define PUSH_PIN 40

/* 波轮方向：原理图上的网络名（KEY_RIGHT / KEY_LEFT）和装好壳以后手感上的
 * "向下拨 / 向上拨"不是一回事。实测 LEFT_PIN 是向下拨、RIGHT_PIN 是向上拨；
 * 以后换壳或换屏装反了，只要把下面两行对调即可（LV_KEY_NEXT 恒等于"焦点往下走"）。 */
#define WHEEL_DOWN_PIN LEFT_PIN
#define WHEEL_UP_PIN RIGHT_PIN
#define LONG_PRESS_THRESHOLD 300 // 定义长按时间阈值
#define SHORT_PRESS_THRESHOLD 10 // 定义短按时间阈值
#define KEY_DEBOUNCE_MS 30       // 波轮/按键抖动过滤：松手后这么久内的按下不算新的一格

lv_indev_t *indev_keypad;

// 在这里设置屏幕尺寸
static const uint32_t screenWidth = 320;
static const uint32_t screenHeight = 240;
// lvgl显示存储数组
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 2];
CST816T cst816t(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN);
hw_timer_t *tim1 = NULL;
TFT_eSPI tft = TFT_eSPI();

/* C 文件（ui.c / event.c）里也能用的串口打印：Arduino 的 Serial 是 C++ 对象，
 * 在 .c 里看不见，所以给界面层一个 C 接口的自检输出口。 */
/* C 文件（ui.c/event.c）用的串口读：返回 -1 表示没有数据 */
extern "C" int exlink_serial_read(void)
{
  if (!Serial.available()) return -1;
  return (int)Serial.read();
}

extern "C" void exlink_log(const char *fmt, ...)
{
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

/* Exlink3.1：数字电源初始化——开机先把输出设成 0 V。
 * 用的是 ExlinkPro 板上的 MP28167（I2C 写参考电压），原版 Exlink 的 MCP4017
 * 数字电位器已不再使用；这里失败也不影响开机，power 任务会继续按 100 ms 重试。 */
void mp28167_init()
{
  if (!mp28167_set_voltage_mv(0, 200))
  {
    Serial.println(F("[mp28167] boot init failed (bus busy / no ACK), power task will retry"));
  }
}

/* 蜂鸣器：按键提示音必须非阻塞。
 * 原来在输入回调里 ledcWrite + delay(100)，等于每读一次按键就把 LVGL 任务
 *（渲染 + 触摸采样）停 100 ms——旋钮节奏、滑动手势、滑动条拖拽都会被拖慢。 */
static esp_timer_handle_t s_beep_timer = NULL;

static void beep_off_cb(void *arg)
{
  (void)arg;
  ledcWrite(1, 0);
}

void buzzer_setup() // 只建一次：定时到点自动停"嘀"
{
  esp_timer_create_args_t args = {};
  args.callback = beep_off_cb;
  args.name = "beep";
  esp_timer_create(&args, &s_beep_timer);
}

void buzzer_task() // 按键提示音：启动后立刻返回
{
  ledcWrite(1, 3);
  if (s_beep_timer)
  {
    esp_timer_stop(s_beep_timer);
    esp_timer_start_once(s_beep_timer, 40 * 1000); // 40 ms
  }
}

void keypad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) // 按键扫描（长按/短按）
{
  /* 波轮：一次拨动 = 只走一格。
   * 原来按住不放会被 LVGL 按 long_press_repeat_time 自动连发 NEXT/PREV，
   * 手感就是"手一碰就窜好几格"。这里两层保证：
   *   1) setup() 把 keypad 驱动的 long_press_time / long_press_repeat_time 拉满，
   *      关掉 LVGL 的自动连发（触摸的长按判定在另一个 indev 上，不受影响）；
   *   2) 本函数只认按下沿：一格用掉后（key_used）必须真的松开、
   *      并且过了 KEY_DEBOUNCE_MS 的抖动窗口，才允许下一格。 */
  static uint32_t last_key = 0;
  static uint32_t push_start_time = 0; // 记录按下的时间
  static bool push_pressed = false;    // 记录PUSH_PIN是否按下
  static bool push_long_sent = false;  // 本次长按是否已经报过"返回"
  static bool key_used = false;        // 这一格是否已经用掉（按住不再重复走）
  static uint32_t release_ms = 0;      // 上一次读到"松开"的时刻
  uint8_t key_state = 0;

  // 读取按键状态
  if (digitalRead(WHEEL_DOWN_PIN) == LOW)
  {
    key_state = 1; // 向下拨一格
  }
  else if (digitalRead(WHEEL_UP_PIN) == LOW)
  {
    key_state = 2; // 向上拨一格
  }
  else if (digitalRead(PUSH_PIN) == LOW)
  {
    if (!push_pressed)
    {
      push_pressed = true;        // 标记PUSH_PIN按下
      push_start_time = millis(); // 记录按下时间
    }
    key_state = 3; // 按下
  }

  if (key_state == 0)
  {
    /* 松开：这一格结束，记下松手时刻（用来滤机械抖动） */
    key_used = false;
    release_ms = millis();
    push_pressed = false;
    push_long_sent = false;
    data->state = LV_INDEV_STATE_REL;
    data->key = last_key;
    return;
  }

  if (!key_used && (millis() - release_ms) >= KEY_DEBOUNCE_MS)
  {
    /* 新的一格：一次机械动作只在这里分配一次按键 */
    key_used = true;
    switch (key_state)
    {
    case 1:
      last_key = LV_KEY_NEXT; // 向下拨一格
      break;
    case 2:
      last_key = LV_KEY_PREV; // 向上拨一格
      break;
    case 3:
      last_key = LV_KEY_ENTER; // 按下 = 确认（松手时 LVGL 补发 CLICKED）
      break;
    default:
      break;
    }
    buzzer_task(); // 一格响一声
    data->state = LV_INDEV_STATE_PR;
  }
  else if (key_used)
  {
    /* 同一格按住不放：保持"按下"（长按判定要用它），但不再产生新的移动 */
    if (key_state == 3 && push_pressed && !push_long_sent &&
        (millis() - push_start_time) >= LONG_PRESS_THRESHOLD)
    {
      push_long_sent = true;
      /* 长按 = 关掉当前焦点所在的功能页（功能继续在后台运行），只报一次；
         键值改成 ESC，松手时 LVGL 不会按 ENTER 再补一次 CLICKED */
      last_key = LV_KEY_ESC;
      lv_event_send(lv_scr_act(), LV_EVENT_LONG_PRESSED, NULL);
    }
    data->state = LV_INDEV_STATE_PR;
  }
  else
  {
    /* 松手后抖动窗口内的按下：当作没按，避免一格变成两格 */
    data->state = LV_INDEV_STATE_REL;
  }

  data->key = last_key; // 更新状态
}

// 屏幕打点函数
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

/* 一帧拿不到可信触摸数据（I2C 忙 / 驱动读失败 / 坐标越界）时：
 * 保持上一次的状态和坐标，不要报"松手"——假松手会当场掐断正在进行的
 * 滑动、拖拽、长按，还会让 LVGL 补发一次 CLICKED。
 * 只有连续多帧都拿不到才当作真松手，避免手指抬起后一直卡在"按下"。 */
static void touch_keep_last(lv_indev_data_t *data, lv_indev_state_t *last_state,
                            lv_point_t *last_point, uint8_t *miss)
{
  if (*last_state == LV_INDEV_STATE_PR && ++(*miss) < 5)
  {
    data->state = *last_state;
    data->point = *last_point;
    return;
  }
  *miss = 0;
  *last_state = LV_INDEV_STATE_REL;
  data->state = *last_state;
  data->point = *last_point;
}

// 触摸屏回调函数（与 INA226/MP28167 共用 Wire，需要总线锁）
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  static lv_indev_state_t last_state = LV_INDEV_STATE_REL;
  static lv_point_t last_point = {0, 0};
  static uint8_t miss = 0;

  if (!app_i2c_bus_lock(pdMS_TO_TICKS(20)))
  {
    touch_keep_last(data, &last_state, &last_point, &miss); /* 总线忙，这一帧没有数据 */
    return;
  }

  TouchInfos tp;
  tp = cst816t.GetTouchInfo();
  app_i2c_bus_unlock();

  /* isValid=false 表示这一帧不可信：CST816T 驱动在 I2C 出错、读超时、
     坐标/手势越界时都会提前返回并把 touching 留成 false —— 直接当成
     "松手"就会把拖拽切成一小段一小段，菜单列永远滑不动。 */
  if (!tp.isValid)
  {
    touch_keep_last(data, &last_state, &last_point, &miss);
    return;
  }
  miss = 0;

  if (!tp.touching)
  {
    last_state = LV_INDEV_STATE_REL;
  }
  else
  {
    last_state = LV_INDEV_STATE_PR;
    last_point.x = 320 - tp.y;
    last_point.y = tp.x;
  }

  /* 松手时也要把最后的位置报回去：LVGL 用它算"按下→抬起"的位移（右滑返回） */
  data->state = last_state;
  data->point = last_point;
}

// 定时器中断服务函数：给 LVGL 提供心跳
void tim1Interrupt()
{
  lv_tick_inc(1);
}

void setup()
{
  esp_task_wdt_delete(NULL); // 删除当前任务的看门狗
  Serial.begin(115200);

  pinMode(1, OUTPUT);
  digitalWrite(1, LOW);
  pinMode(3, OUTPUT);
  ledcAttachPin(3, 1);
  ledcSetup(1, 1000, 8);
  ledcWrite(1, 0);
  buzzer_setup();

  pinMode(RIGHT_PIN, INPUT_PULLUP);
  pinMode(LEFT_PIN, INPUT_PULLUP);
  pinMode(PUSH_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(2, ADC_11db);
  analogSetPinAttenuation(4, ADC_11db);

  tft.init();
  tft.setRotation(3); // 设置显示方向
  cst816t.begin();    // 初始化触摸屏

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 2);

  static lv_disp_drv_t disp_drv; // 初始化显示器
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.sw_rotate = 1;             // 屏幕镜像
  disp_drv.rotated = LV_DISP_ROT_180; // 屏幕旋转
  lv_disp_drv_register(&disp_drv);    // 注册显示屏

  static lv_indev_drv_t indev_drv1;
  lv_indev_drv_init(&indev_drv1);
  indev_drv1.type = LV_INDEV_TYPE_POINTER; // 设置为触摸屏类型
  indev_drv1.read_cb = my_touchpad_read;   // 注册回调函数
  lv_indev_drv_register(&indev_drv1);      // 注册输入设备

  static lv_indev_drv_t indev_drv2;
  lv_indev_drv_init(&indev_drv2);
  indev_drv2.read_cb = keypad_read;                  // 注册回调函数
  indev_drv2.type = LV_INDEV_TYPE_KEYPAD;            // 设置为按键类型
  /* 关掉 LVGL 的按键自动连发：按住 NEXT/PREV 时不再每隔 long_press_repeat_time
     重复走一格（"拨一次 = 走一格"）。触摸长按判定用的是另一个 indev（indev_drv1），
     时间仍是默认 400 ms，不受影响。 */
  indev_drv2.long_press_time = 0xFFFF;
  indev_drv2.long_press_repeat_time = 0xFFFF;
  indev_keypad = lv_indev_drv_register(&indev_drv2); // 注册输入设备

  // 启动定时器为lvgl提供心跳时钟
  tim1 = timerBegin(0, 80, true);
  timerAttachInterrupt(tim1, tim1Interrupt, true);
  timerAlarmWrite(tim1, 1000, true);
  timerAlarmEnable(tim1);

  ledcWrite(1, 3); // 开机提示音
  Wire.begin(I2C_SDA, I2C_SCL);

  // ---- 应用层：互斥量 / 流缓冲 ----
  app_init();

  Serial.println("[3.0] ui_init start");
  ui_init(); // 初始化ui界面（开机动画 -> 主菜单）
  Serial.println("[3.0] ui_init done");

  // ---- 启动全部功能任务 ----
  app_start_tasks();
  Serial.println("[3.0] all tasks started");

  mp28167_init(); // 数字电源复位到 0 V
  ledcWrite(1, 0);
}

void loop()
{
  // 所有功能都在各自的 FreeRTOS 任务里运行，这里只让 Arduino loop 任务休眠，
  // 保证看门狗与系统任务有机会调度。
  vTaskDelay(pdMS_TO_TICKS(1000));
}
