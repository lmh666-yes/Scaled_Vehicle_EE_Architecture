/*
 * ESP32-S3 上电自检（bring-up）
 *
 * 一次跑完，验证五件事：
 *   1. 芯片 / Flash / PSRAM 是否与 N16R8 一致
 *   2. 8MB PSRAM 能否真正读写（摄像头图像缓冲靠它）
 *   3. TWAI(CAN) 驱动能否装上（GPIO4=TX, GPIO5=RX, 1 Mbps）
 *   4. GPIO 输出能否翻转（示波器/万用表可测）
 *   5. 复位原因（判断是不是被掉电/看门狗复位）
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "BRINGUP";

#define PIN_TICK   GPIO_NUM_2    /* 心跳输出，可接 LED / 示波器 */
#define CAN_TX_PIN GPIO_NUM_4
#define CAN_RX_PIN GPIO_NUM_5

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "POWERON (上电)";
    case ESP_RST_EXT:      return "EXT (外部复位脚)";
    case ESP_RST_SW:       return "SW (软件复位)";
    case ESP_RST_PANIC:    return "PANIC (崩溃)";
    case ESP_RST_INT_WDT:  return "INT_WDT (中断看门狗)";
    case ESP_RST_TASK_WDT: return "TASK_WDT (任务看门狗)";
    case ESP_RST_WDT:      return "WDT (其他看门狗)";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP (深睡唤醒)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (掉压!)";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
    }
}

static bool psram_rw_test(void)
{
    const size_t sz = 4 * 1024 * 1024;   /* 4 MB */
    uint8_t *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        ESP_LOGE(TAG, "PSRAM: malloc 4MB FAILED");
        return false;
    }
    for (size_t i = 0; i < sz; i++) {
        p[i] = (uint8_t)(i * 7 + (i >> 13));
    }
    size_t bad = 0;
    for (size_t i = 0; i < sz; i++) {
        if (p[i] != (uint8_t)(i * 7 + (i >> 13))) {
            if (bad < 3) ESP_LOGE(TAG, "PSRAM mismatch @%u", (unsigned)i);
            bad++;
        }
    }
    heap_caps_free(p);
    ESP_LOGI(TAG, "PSRAM: 4MB write+read %s (errors=%u)", bad == 0 ? "PASS" : "FAIL", (unsigned)bad);
    return bad == 0;
}

static void twai_probe(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWAI: driver_install FAILED (0x%x) —— 检查 GPIO%d/GPIO%d 是否被占用",
                 err, CAN_TX_PIN, CAN_RX_PIN);
        return;
    }
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TWAI: start FAILED (0x%x)", err);
        twai_driver_uninstall();
        return;
    }
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK) {
        ESP_LOGI(TAG, "TWAI: STARTED 1Mbps TX=GPIO%d RX=GPIO%d state=%d",
                 CAN_TX_PIN, CAN_RX_PIN, (int)st.state);
    }
    ESP_LOGI(TAG, "TWAI: 注意 —— 没有收发器时 state 会停在 BUS_OFF，这是正常的");
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-S3 上电自检  (build %s %s)", __DATE__, __TIME__);
    ESP_LOGI(TAG, "========================================");

    /* ---------- 1. 芯片 ---------- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "[1] target=%s  cores=%d  rev=v%d.%d  features=0x%x",
             CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100, chip.features);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "[1] flash = %u MB", (unsigned)(flash_size / (1024 * 1024)));
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "[1] psram = %u MB  (initialized=%d)",
             (unsigned)(psram_size / (1024 * 1024)), (int)esp_psram_is_initialized());

    ESP_LOGI(TAG, "[1] internal RAM: total %u KB, free %u KB",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "[1] PSRAM heap : total %u KB, free %u KB",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* ---------- 2. PSRAM 读写 ---------- */
    ESP_LOGI(TAG, "[2] PSRAM 读写测试 ...");
    psram_rw_test();

    /* ---------- 3. TWAI ---------- */
    ESP_LOGI(TAG, "[3] TWAI 初始化测试 ...");
    twai_probe();

    /* ---------- 4. GPIO 心跳 ---------- */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TICK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "[4] GPIO%d 心跳输出已启用", PIN_TICK);

    /* ---------- 5. 复位原因 ---------- */
    ESP_LOGI(TAG, "[5] reset reason = %s", reset_reason_str(esp_reset_reason()));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " 自检结束，进入心跳循环（每 1 秒一次）");
    ESP_LOGI(TAG, "========================================");

    uint32_t tick = 0;
    int level = 0;
    while (1) {
        level = !level;
        gpio_set_level(PIN_TICK, level);
        tick++;
        if (tick % 5 == 0) {
            ESP_LOGI(TAG, "heartbeat %u  free_internal=%u KB  free_psram=%u KB",
                     (unsigned)tick,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
