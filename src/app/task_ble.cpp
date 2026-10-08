/**
 * @file  task_ble.cpp
 * @brief BLE 无线串口任务（Nordic UART 服务）
 *
 * 首次打开时初始化 BLE；之后开关只是启动/停止广播与数据转发，
 * 断线会自动重新广播。接收到的数据写入流缓冲，由界面定时器显示。
 *
 * 按需运行：蓝牙没打开时阻塞在任务通知上（CPU 占用 0），
 * 也不做 BLEDevice::init()——协议栈是"第一次按开才初始化"；
 * 打开后按 200 ms 检查连接状态（断线重播），连接/断开事件会立刻叫醒它。
 */
#include "app_shared.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define BLE_PERIOD_MS 200 /* 打开时检查"断线重播"的周期 */
#define BLE_READV_MS  300 /* 断线后延迟重播，避免和协议栈抢时间 */

static BLEServer *pServer = NULL;
static BLECharacteristic *pTxCharacteristic = NULL;
static BLECharacteristic *pRxCharacteristic = NULL;
static bool s_inited = false;

class ExlinkServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *server) { (void)server; app_ble_set_connected(true); }
    void onDisconnect(BLEServer *server) { (void)server; app_ble_set_connected(false); }
};

class ExlinkRxCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *c)
    {
        std::string rxValue = c->getValue();
        if (rxValue.length() > 0) app_ble_push(rxValue.c_str());
    }
};

/* 供串口任务转发使用 */
void app_ble_notify(const char *data, size_t len)
{
    if (!pTxCharacteristic || !data || len == 0) return;
    pTxCharacteristic->setValue((uint8_t *)data, len);
    pTxCharacteristic->notify();
}

static void ble_init(void)
{
    BLEDevice::init("Exlink3.0");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ExlinkServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX,
                                                       BLECharacteristic::PROPERTY_NOTIFY);
    pTxCharacteristic->addDescriptor(new BLE2902());
    pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX,
                                                       BLECharacteristic::PROPERTY_WRITE);
    pRxCharacteristic->setCallbacks(new ExlinkRxCallbacks());
    pService->start();
    pServer->getAdvertising()->start();
    s_inited = true;
    app_ble_push("Exlink3.0 BLE advertising...\n");
}

static void ble_task(void *arg)
{
    (void)arg;
    bool last_on = false;

    for (;;)
    {
        bool on = app_ble_on();

        if (on && !s_inited) ble_init();

        if (s_inited)
        {
            if (on && !last_on)
            {
                pServer->getAdvertising()->start();
                app_ble_push("BLE advertising...\n");
            }
            else if (!on && last_on)
            {
                pServer->getAdvertising()->stop();
                app_ble_set_connected(false);
                app_ble_push("BLE stopped.\n");
            }
            else if (on && !app_ble_connected() && last_on)
            {
                /* 断开后重新广播 */
                vTaskDelay(pdMS_TO_TICKS(BLE_READV_MS));
                pServer->startAdvertising();
                app_ble_push("BLE re-advertising...\n");
            }
        }

        last_on = on;

        if (on) app_task_wait(APP_TASK_BLE, BLE_PERIOD_MS, true); /* 开着：200 ms 巡检一次 */
        else    app_task_wait(APP_TASK_BLE, 0, false);            /* 关着：阻塞，不占 CPU */
    }
}

void app_task_ble_start(void)
{
    app_task_start(ble_task, "ble", 10240, 2, 0, APP_TASK_BLE);
}
