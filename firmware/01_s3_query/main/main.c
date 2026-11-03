/*
 * ESP32-S3-N16R8 硬件查询程序（ESP-IDF v6.1）
 *
 * 一次性查询并打印：
 *   芯片 / 固件 / Flash / PSRAM / 内存 / 分区表 / MAC / 温度 / 复位原因
 * 之后每 5 秒刷新一次运行状态。
 *
 * 与 00_s3_bringup 的区别：这里只读不写，不动 TWAI、不做内存读写测试，
 * 适合拿到新板子或换 Flash 后核对硬件参数。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---------- 字符串转换 ---------- */

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32P4: return "ESP32-P4";
    default:           return "未知型号";
    }
}

static void format_features(uint32_t features, char *out, size_t out_len)
{
    out[0] = '\0';
    if (features & CHIP_FEATURE_WIFI_BGN)      strcat(out, "WiFi-2.4G ");
    if (features & CHIP_FEATURE_BLE)           strcat(out, "BLE ");
    if (features & CHIP_FEATURE_BT)            strcat(out, "BT-Classic ");
    if (features & CHIP_FEATURE_IEEE802154)    strcat(out, "IEEE802.15.4 ");
    if (features & CHIP_FEATURE_EMB_FLASH)     strcat(out, "内置Flash ");
    if (features & CHIP_FEATURE_EMB_PSRAM)     strcat(out, "内置PSRAM ");
    if (out[0] == '\0') {
        snprintf(out, out_len, "无");
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON 上电复位";
    case ESP_RST_EXT:       return "EXT 外部复位脚";
    case ESP_RST_SW:        return "SW 软件复位";
    case ESP_RST_PANIC:     return "PANIC 程序崩溃";
    case ESP_RST_INT_WDT:   return "INT_WDT 中断看门狗";
    case ESP_RST_TASK_WDT:  return "TASK_WDT 任务看门狗";
    case ESP_RST_WDT:       return "WDT 其他看门狗";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP 深睡唤醒";
    case ESP_RST_BROWNOUT:  return "BROWNOUT 掉压复位";
    case ESP_RST_SDIO:      return "SDIO 复位";
    case ESP_RST_USB:       return "USB 复位";
    case ESP_RST_JTAG:      return "JTAG 复位";
    case ESP_RST_EFUSE:     return "EFUSE 错误复位";
    case ESP_RST_PWR_GLITCH:return "PWR_GLITCH 电源毛刺复位";
    case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP CPU 锁死复位";
    case ESP_RST_UNKNOWN:   return "UNKNOWN 未知";
    default:                return "UNKNOWN 未知";
    }
}

static const char *part_type_name(esp_partition_type_t type)
{
    switch (type) {
    case ESP_PARTITION_TYPE_APP:             return "app";
    case ESP_PARTITION_TYPE_DATA:            return "data";
    case ESP_PARTITION_TYPE_BOOTLOADER:      return "boot";
    case ESP_PARTITION_TYPE_PARTITION_TABLE: return "ptable";
    default:                                 return "other";
    }
}

static const char *part_subtype_name(esp_partition_type_t type, esp_partition_subtype_t subtype)
{
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
        case ESP_PARTITION_SUBTYPE_APP_TEST:    return "test";
        default:                                return "app";
        }
    }
    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA:       return "ota";
        case ESP_PARTITION_SUBTYPE_DATA_PHY:       return "phy";
        case ESP_PARTITION_SUBTYPE_DATA_NVS:       return "nvs";
        case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:  return "coredump";
        case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:  return "nvs_keys";
        case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM:  return "efuse";
        case ESP_PARTITION_SUBTYPE_DATA_FAT:       return "fat";
        case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:    return "spiffs";
        case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:  return "littlefs";
        default:                                   return "data";
        }
    }
    return "?";
}

/* ---------- 各项查询 ---------- */

static void query_system(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const esp_app_desc_t *app = esp_app_get_description();
    char features[128];

    format_features(chip.features, features, sizeof(features));

    printf("[芯片 / 固件]\n");
    printf("  芯片型号 : %s\n", chip_model_name(chip.model));
    printf("  内核数量 : %u\n", (unsigned)chip.cores);
    printf("  芯片版本 : v%u.%u\n",
           (unsigned)(chip.revision / 100U), (unsigned)(chip.revision % 100U));
    printf("  芯片特性 : %s\n", features);
    printf("  CPU 主频 : %d MHz（编译配置）\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    printf("  IDF 版本 : %s\n", esp_get_idf_version());
    printf("  工程名称 : %s\n", app->project_name);
    printf("  固件版本 : %s\n", app->version);
    printf("  编译时间 : %s %s\n", app->date, app->time);
}

static void query_flash(void)
{
    uint32_t flash_id = 0;
    uint32_t flash_size = 0;
    uint32_t physical_size = 0;
    uint64_t unique_id = 0;
    esp_err_t err;

    printf("[Flash]\n");

    err = esp_flash_read_id(esp_flash_default_chip, &flash_id);
    if (err == ESP_OK) {
        printf("  JEDEC ID : 0x%06" PRIX32 "\n", flash_id);
    } else {
        printf("  JEDEC ID : 读取失败（%s）\n", esp_err_to_name(err));
    }

    err = esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (err == ESP_OK) {
        printf("  配置容量 : %" PRIu32 " MB\n", (uint32_t)(flash_size / (1024U * 1024U)));
    } else {
        printf("  配置容量 : 读取失败（%s）\n", esp_err_to_name(err));
    }

    err = esp_flash_get_physical_size(esp_flash_default_chip, &physical_size);
    if (err == ESP_OK) {
        printf("  物理容量 : %" PRIu32 " MB\n", (uint32_t)(physical_size / (1024U * 1024U)));
    } else {
        printf("  物理容量 : 读取失败（%s）\n", esp_err_to_name(err));
    }

    err = esp_flash_read_unique_chip_id(esp_flash_default_chip, &unique_id);
    if (err == ESP_OK) {
        printf("  唯一 ID  : 0x%016" PRIX64 "\n", unique_id);
    } else {
        printf("  唯一 ID  : 当前 Flash 不支持（%s）\n", esp_err_to_name(err));
    }

    printf("  驱动状态 : %s\n",
           esp_flash_chip_driver_initialized(esp_flash_default_chip) ? "已初始化" : "未初始化");
}

static void query_memory(void)
{
    size_t psram_size = esp_psram_get_size();

    printf("[内存 / PSRAM]\n");
    printf("  内部 RAM : 总 %6u KB | 空闲 %6u KB | 最大连续块 %6u KB\n",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U));
    printf("  PSRAM    : 总 %6u KB | 空闲 %6u KB | 最大连续块 %6u KB\n",
           (unsigned)(psram_size / 1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
    printf("  PSRAM 驱动 : %s\n", esp_psram_is_initialized() ? "已初始化" : "未初始化");
    printf("  历史最低空闲堆 : %u KB\n",
           (unsigned)(esp_get_minimum_free_heap_size() / 1024U));
}

static void query_partitions(void)
{
    unsigned count = 0;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    printf("[分区表]\n");
    printf("  %-12s %-7s %-9s %-11s %10s\n",
           "名称", "类型", "子类型", "起始地址", "大小");

    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        char subtype[24];

        if (p->type == ESP_PARTITION_TYPE_APP &&
            p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
            p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            snprintf(subtype, sizeof(subtype), "ota_%u",
                     (unsigned)(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
        } else {
            snprintf(subtype, sizeof(subtype), "%s",
                     part_subtype_name(p->type, p->subtype));
        }

        printf("  %-12s %-7s %-9s 0x%08" PRIx32 " %8" PRIu32 " KB%s\n",
               p->label,
               part_type_name(p->type),
               subtype,
               (uint32_t)p->address,
               (uint32_t)(p->size / 1024U),
               p->encrypted ? "  加密" : "");

        count++;
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (count == 0) {
        printf("  （未找到分区）\n");
    }
    printf("  共 %u 个分区\n", count);
}

static void print_mac(const char *label, esp_mac_type_t type)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, type);

    if (err == ESP_OK) {
        printf("  %-8s : %02X:%02X:%02X:%02X:%02X:%02X\n",
               label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("  %-8s : 读取失败（%s）\n", label, esp_err_to_name(err));
    }
}

static void query_mac(void)
{
    printf("[MAC 地址]\n");
    print_mac("基础 MAC", ESP_MAC_BASE);
    print_mac("WiFi STA", ESP_MAC_WIFI_STA);
    print_mac("蓝牙 MAC", ESP_MAC_BT);
}

static void query_temperature(void)
{
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err;

    printf("[温度]\n");

    err = temperature_sensor_install(&cfg, &tsens);
    if (err != ESP_OK) {
        printf("  芯片温度 : 驱动安装失败（%s）\n", esp_err_to_name(err));
        return;
    }

    err = temperature_sensor_enable(tsens);
    if (err != ESP_OK) {
        printf("  芯片温度 : 使能失败（%s）\n", esp_err_to_name(err));
    } else {
        float celsius = 0.0f;
        err = temperature_sensor_get_celsius(tsens, &celsius);
        if (err == ESP_OK) {
            printf("  芯片温度 : %.1f °C\n", celsius);
        } else {
            printf("  芯片温度 : 读取失败（%s）\n", esp_err_to_name(err));
        }
        temperature_sensor_disable(tsens);
    }

    temperature_sensor_uninstall(tsens);
}

static void query_runtime(void)
{
    printf("[运行状态]\n");
    printf("  复位原因 : %s\n", reset_reason_name(esp_reset_reason()));
    printf("  已运行   : %" PRIu64 " ms\n",
           (uint64_t)(esp_timer_get_time() / 1000));
    printf("  任务数量 : %u\n", (unsigned)uxTaskGetNumberOfTasks());
}

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" ESP32-S3-N16R8 硬件查询\n");
    printf("========================================\n");

    query_system();
    query_flash();
    query_memory();
    query_partitions();
    query_mac();
    query_temperature();
    query_runtime();

    printf("========================================\n");
    printf(" 查询完成，每 5 秒刷新运行状态\n");
    printf("========================================\n");

    unsigned n = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        n++;
        printf("[%u] 运行 %" PRIu64 " s | 内部空闲 %u KB | PSRAM 空闲 %u KB | 任务 %u\n",
               n,
               (uint64_t)(esp_timer_get_time() / 1000000),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
               (unsigned)uxTaskGetNumberOfTasks());
    }
}
