# ESP32-S3 学习教程（从 F407 的视角）

> 写给已经会 STM32F407 标准库、但第一次碰 ESP-IDF 的人。
> 每一节都能跑、都能看到现象，不是抄概念。
>
> 环境：ESP-IDF v6.1 / ESP32-S3-N16R8 / 串口 COM17

---

## 0. 先搞清：ESP32 和 F407 哪里像、哪里不像

**一句话：外设概念像，软件栈不像。**

### 像的部分（学会 F407 就能直接迁移的）

| 概念 | F407 | ESP32-S3 |
|---|---|---|
| GPIO 输出/输入 | `GPIO_Init()` | `gpio_config()` |
| 中断 | `NVIC_EnableIRQ()` + `EXTI` | `gpio_install_isr_service()` + 回调 |
| 定时器 | `TIM2` | `gptimer`（通用）/ `esp_timer`（软件） |
| 串口 | `USART1` | `uart` 驱动 |
| I2C | `I2C1` | `i2c` 驱动 |
| 编码器 | `TIM` 编码器模式 | **`PCNT`**（脉冲计数器） |
| CAN | `CAN1` | **`TWAI`**（名字不同，逻辑一模一样） |
| DMA | `DMA2` | `GDMA` |
| 看门狗 | `IWDG` | `Task WDT` + `esp_task_wdt` |
| ADC | `ADC1/2/3` | `ADC1/2`（`adc_oneshot`） |
| PWM | `TIM1 CH1` | **`LEDC`**（LED 控制器）/ `MCPWM` |

**这些外设你在 F407 上怎么想，在 ESP32 上基本一样想**，只是 API 名字换了。

### 不像的部分（这才是你要花时间的地方）

| 维度 | F407 | ESP32-S3 |
|---|---|---|
| **内核** | ARM Cortex-M4F（单核，168MHz） | Xtensa LX7 **双核**（240MHz）+ LP 核 |
| **RTOS** | 可选，一般裸机 | **强制**，IDF 本身就跑在 FreeRTOS 上 |
| **开发框架** | 寄存器 / 标准库 / HAL | **ESP-IDF**（组件化 + CMake） |
| **程序入口** | `main()` | `app_main()`，它只是**一个任务**，不是 main |
| **构建** | Keil 点编译 | `idf.py build`（CMake + Ninja） |
| **烧录** | SWD / J-Link | UART 或 USB（`idf.py flash`） |
| **调试** | 断点、看寄存器 | `idf.py monitor` + 日志（内置 JTAG 可断点） |
| **内存** | 192KB SRAM，无扩展 | 512KB SRAM + **8MB PSRAM** |
| **启动** | 上电就跑 | Bootloader → 分区表 → 你的 app |
| **特有外设** | FSMC、DCMI、CAN | **WiFi/BT**、RMT、LCD_CAM、I2S、触摸 |

### 三个最容易踩的认知差

**① ESP32 没有"裸机"，一上来就是 RTOS**

F407 里你写：

```c
int main(void) {
    while (1) { /* 你的全部逻辑 */ }
}
```

ESP32 里你写：

```c
void app_main(void) {
    /* 这里只是"第一个任务"的入口，函数返回了程序照样跑 */
}
```

`app_main` **返回后系统不会停**，其他任务还在跑。这一点和 F407 完全不同。

**② 双核：任务可以指定跑在哪个核**

```c
xTaskCreatePinnedToCore(task, "t", 4096, NULL, 5, NULL, 0);  // 第 9 个参数 = 核号
```

- 核 0（PRO_CPU）：默认跑 WiFi/BT 协议栈
- 核 1（APP_CPU）：`app_main` 默认在这

你的项目里，**WiFi 任务固定核 0，CAN/状态机固定核 1**，就是为了不让协议栈抖动影响实时性。

**③ "驱动层"多了一层抽象**

F407 标准库：`RCC_APB1PeriphClockCmd(...)` → `GPIO_Init(...)` → 直接写寄存器。

ESP-IDF：`driver` 层（如 `uart_driver_install`）→ `hal` 层 → 寄存器。

好处是跨芯片可移植，坏处是**你得习惯"先 install 再 start"这套流程**（蓝牙、I2C、TWAI、UART 都是这个模式）。

---

## 1. 工程结构：一个 ESP-IDF 工程由什么组成

```
00_s3_bringup/
├── CMakeLists.txt           ← 项目级，只做一件事：project(名字)
├── sdkconfig.defaults       ← 默认配置（第一次编译时生成 sdkconfig）
├── sdkconfig                ← 实际生效的配置（menuconfig 改的就是它）
├── main/
│   ├── CMakeLists.txt       ← 声明源文件、依赖组件
│   └── main.c               ← 你的代码
└── build/                   ← 编译产物（可删，删了重编）
```

**和 Keil 工程的区别：**

| | Keil | ESP-IDF |
|---|---|---|
| 加文件 | 右键 Add Existing File | 改 `main/CMakeLists.txt` 的 `SRCS` |
| 加依赖库 | 改 C/C++ 选项卡 | `idf_component_register(... REQUIRES xxx)` |
| 配置 | Options for Target | `menuconfig`（存到 sdkconfig） |
| 干净重编 | Rebuild | 删 `build/` 目录 |

**关键的 `main/CMakeLists.txt`：**

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer nvs_flash)
```

- `SRCS`：源文件列表（加文件就写这里）
- `REQUIRES`：要用哪些组件（不写会链接失败）

---

## 2. 日志：ESP_LOGI 和 printf 的区别

**目标**：搞懂为什么 IDF 里到处是 `ESP_LOGI` 而不是 `printf`。

```c
#include "esp_log.h"

static const char *TAG = "DEMO";

void app_main(void)
{
    ESP_LOGE(TAG, "这是 ERROR，红色");
    ESP_LOGW(TAG, "这是 WARN，黄色");
    ESP_LOGI(TAG, "这是 INFO，绿色");
    ESP_LOGD(TAG, "这是 DEBUG，默认不显示");
    ESP_LOGV(TAG, "这是 VERBOSE，默认不显示");
}
```

**你会看到：**

```
E (312) DEMO: 这是 ERROR，红色
W (315) DEMO: 这是 WARN，黄色
I (318) DEMO: 这是 INFO，绿色
```

那个 `(312)` 是**系统启动后的毫秒数**——这是排查时序问题最重要的信息（比 printf 强太多）。

**和 F407 对照：**

| | F407 | ESP32 |
|---|---|---|
| 打印 | `printf` → `fputc` 重定向到 USART | `ESP_LOGI` 直接输出 |
| 分级 | 自己写 | 内置 5 级，编译期可裁剪 |
| 时间戳 | 自己加 | 自动带毫秒 |
| 关掉某些日志 | 注释掉 | `menuconfig` 里设级别 |

**建议**：项目里全部用 `ESP_LOGx`，不要用 `printf`。TAG 用大写的模块名（`"CAN"`、`"MOTOR"`、`"POWER"`）。

**练习**：把 bringup 工程里的日志 TAG 从 `BRINGUP` 改成你的名字，重编译烧录，看输出变化。

---

## 3. GPIO：输出与输入

**目标**：点亮一个 LED / 读一个按键。

### 输出

```c
#include "driver/gpio.h"

#define PIN_LED GPIO_NUM_2

gpio_config_t io = {
    .pin_bit_mask = (1ULL << PIN_LED),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&io);

gpio_set_level(PIN_LED, 1);   // 高
gpio_set_level(PIN_LED, 0);   // 低
```

### 输入 + 上拉

```c
#define PIN_KEY GPIO_NUM_0    // 板上的 BOOT 键就在 GPIO0

gpio_config_t in = {
    .pin_bit_mask = (1ULL << PIN_KEY),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&in);

int level = gpio_get_level(PIN_KEY);   // 按下 = 0
```

**和 F407 对照：**

| F407 | ESP32 |
|---|---|
| `RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE)` | 不用开时钟（自动管理） |
| `GPIO_Init(GPIOA, &cfg)` | `gpio_config(&io)` |
| `GPIO_SetBits / ResetBits` | `gpio_set_level(引脚, 0/1)` |
| `GPIO_ReadInputDataBit` | `gpio_get_level(引脚)` |

**注意**：ESP32 用**引脚编号**（`GPIO_NUM_2`），不是 `GPIOA Pin2` 那种分组。

**练习**：让 GPIO2 每 500ms 翻转一次，用万用表量电压应该在 0V/3.3V 之间跳。

---

## 4. FreeRTOS：任务

**目标**：理解为什么 ESP32 里"main 只是第一个任务"。

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 任务函数：必须是这个签名，且不能返回（要么死循环，要么 vTaskDelete(NULL)）
void blink_task(void *arg)
{
    int level = 0;
    while (1) {
        level = !level;
        gpio_set_level(GPIO_NUM_2, level);
        ESP_LOGI("BLINK", "level=%d", level);
        vTaskDelay(pdMS_TO_TICKS(500));   // 关键：必须让出 CPU
    }
}

void app_main(void)
{
    xTaskCreate(blink_task, "blink", 4096, NULL, 5, NULL);
    // 参数：函数, 名字, 栈大小(字节), 参数, 优先级(0-25), 句柄

    ESP_LOGI("MAIN", "任务已创建，app_main 结束了程序照样跑");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI("MAIN", "主任务还活着，剩余堆 %u",
                 (unsigned)(esp_get_free_heap_size() / 1024));
    }
}
```

**你会看到**：BLINK 每 500ms 打一次，MAIN 每 1000ms 打一次，**两个互不干扰**。

**和 F407 对照：**

| | F407 裸机 | ESP32 |
|---|---|---|
| "并发" | 主循环里轮询 / 状态机 | 真并发（RTOS 调度） |
| 延时 | `delay_ms()`（阻塞 CPU） | `vTaskDelay()`（让出 CPU 给别的任务） |
| `while(1)` | 正常 | ⚠️ 没有 delay 的 `while(1)` 会**饿死低优先级任务**，甚至触发看门狗 |

**三个必记的坑：**

1. **任务栈单位是字节**，不是字。4096 = 4KB，一般够；但用 `printf` 或大数组要给 8192。
2. **优先级数字越大越高**（0-25），和 Cortex-M 中断优先级**相反**（那边数值越小越高）。
3. **`while(1)` 里必须 `vTaskDelay` 或阻塞等待**，否则空闲任务跑不到，看门狗报警。

**练习**：创建两个任务，一个 500ms 翻转 GPIO2，一个 200ms 打印计数。观察它们是否"同时"跑。

---

## 5. 队列：任务之间传数据

**目标**：一个任务收数据，另一个任务处理——这是本项目所有模块通信的基础。

```c
#include "freertos/queue.h"

typedef struct {
    uint32_t tick;
    int value;
} msg_t;

static QueueHandle_t q;

void producer_task(void *arg)
{
    msg_t m = {0};
    while (1) {
        m.tick++;
        m.value = m.tick * 10;
        xQueueSend(q, &m, portMAX_DELAY);      // 发到队列
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void consumer_task(void *arg)
{
    msg_t m;
    while (1) {
        if (xQueueReceive(q, &m, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI("CONS", "收到 tick=%u value=%d", m.tick, m.value);
        }
    }
}

void app_main(void)
{
    q = xQueueCreate(10, sizeof(msg_t));     // 深度 10，每项 sizeof(msg_t)
    xTaskCreate(producer_task, "prod", 4096, NULL, 5, NULL);
    xTaskCreate(consumer_task, "cons", 4096, NULL, 4, NULL);
}
```

**为什么用队列而不是全局变量？**

| 全局变量 | 队列 |
|---|---|
| 要自己加锁（信号量/临界区） | 自带互斥 |
| 生产者快、消费者慢会丢数据 | 满了会阻塞（或返回失败） |
| 看不出数据流 | 一眼看出谁给谁发 |

**你项目里的用法**：CAN 接收中断 → 队列 → 解析任务 → 另一个队列 → 控制任务。**中断里绝对不能做耗时操作，只能塞队列。**

**练习**：把队列深度改成 2，生产者 50ms 发一次、消费者 500ms 收一次，观察 `xQueueSend` 什么时候返回 `errQUEUE_FULL`。

---

## 6. 定时器：esp_timer 与 gptimer

**目标**：搞懂两种定时器的用途区别。

### `esp_timer`：软件定时器（回调在任务上下文）

```c
#include "esp_timer.h"

static void on_timer(void *arg)
{
    static int n = 0;
    ESP_LOGI("TMR", "第 %d 次", ++n);
}

void app_main(void)
{
    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .name = "my_timer",
    };
    esp_timer_handle_t h;
    esp_timer_create(&args, &h);
    esp_timer_start_periodic(h, 1000000);    // 单位：微秒，1000000 = 1 秒
}
```

### `gptimer`：硬件定时器（可产生精确定时 / 中断）

```c
#include "driver/gptimer.h"

static bool on_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *d, void *arg)
{
    static int n = 0;
    n++;
    return false;    // 返回 true 会唤醒被阻塞的任务
}
```

**怎么选：**

| | `esp_timer` | `gptimer` |
|---|---|---|
| 精度 | 微秒级（但受任务调度影响） | 硬件级，最准 |
| 用途 | 周期性任务、超时判断 | PWM 相关、精确定时、捕获 |
| 复杂度 | 简单 | 稍复杂 |
| 你的项目 | 心跳 1Hz、超时检测 | 别用它做控制环 |

**⚠️ 关键认知：ESP32 上做 1kHz 控制环，不用定时器中断，用任务 + `vTaskDelayUntil`。**

原因：FreeRTOS 的 tick 是 1ms（`CONFIG_FREERTOS_HZ=1000`），1kHz 控制可以用"每 tick 一次"的任务实现，但抖动在几十微秒级——**比 F407 的硬件定时器中断差**。

这也是为什么**动力域（1kHz 速度环）必须留在 F407**，不能搬到 ESP32。

**练习**：用 `esp_timer` 每 3 秒打印一次当前堆大小，观察有没有内存泄漏。

---

## 7. 中断：GPIO 中断 + 队列

**目标**：中断里只做最少的事（塞队列），处理放任务里。

```c
#include "driver/gpio.h"
#include "freertos/queue.h"

static QueueHandle_t gpio_q;

// 中断服务函数（ISR）：必须短，不能阻塞
static void IRAM_ATTR gpio_isr(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_q, &gpio_num, NULL);   // 注意是 FromISR 版本
}

void app_main(void)
{
    gpio_q = xQueueCreate(10, sizeof(uint32_t));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,          // 下降沿（按键按下）
    };
    gpio_config(&io);

    gpio_install_isr_service(0);                 // 先装服务（全局一次）
    gpio_isr_handler_add(GPIO_NUM_0, gpio_isr, (void *)GPIO_NUM_0);

    uint32_t num;
    while (1) {
        if (xQueueReceive(gpio_q, &num, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI("ISR", "GPIO %u 触发（在任务里处理）", (unsigned)num);
            // 这里可以做耗时的事
        }
    }
}
```

**和 F407 对照：**

| F407 | ESP32 |
|---|---|
| `EXTI0_IRQHandler()` | `gpio_isr_handler_add()` 注册回调 |
| `NVIC_SetPriority()` | `gpio_install_isr_service(优先级)` |
| 手动清中断标志 | 自动处理 |

**三个必记的点：**

1. **ISR 里只能用 `...FromISR` 结尾的 API**（`xQueueSendFromISR`），用普通的会崩。
2. **ISR 要加 `IRAM_ATTR`**（放内存里，不从 Flash 取指），否则 Flash 操作期间触发中断会崩。
3. **ISR 里不能 `vTaskDelay`、不能 `printf`**——只能塞队列/给信号量。

**练习**：按住 BOOT 键（GPIO0）看会触发几次——你会发现**按键抖动导致触发很多次**，这就是为什么实际项目要做软件消抖（连续 3 次采样一致才认）。

---

## 8. UART：串口收发

**目标**：开一个自己的串口（比如接 LoRa 模块）。

```c
#include "driver/uart.h"

#define UART_PORT   UART_NUM_1        // UART1（UART0 是日志口，别抢）
#define UART_TX     17
#define UART_RX     18
#define UART_BUF    1024

void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_BUF * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void app_main(void)
{
    uart_init();

    uint8_t buf[128];
    while (1) {
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            ESP_LOGI("UART", "收到 %d 字节", n);
            uart_write_bytes(UART_PORT, buf, n);      // 回显
        }
    }
}
```

**和 F407 对照：**

| F407 | ESP32 |
|---|---|
| `USART_Init()` | `uart_param_config()` |
| `USART_SendData()` | `uart_write_bytes()` |
| `USART_ReceiveData()` | `uart_read_bytes()` |
| 中断 + 环形缓冲自己写 | `uart_driver_install()` 自带环形缓冲 |

**你的项目里**：LoRa E22 就接在这个口上，9600 8N1。

**练习**：短接 TX 和 RX（GPIO17 接 GPIO18），发什么收什么，验证串口通。

---

## 9. I2C：读传感器（以 INA226 为例）

**目标**：用新版 I2C 驱动读一个寄存器。

⚠️ **v6.1 用的是新 I2C API**（`i2c_new_master_bus`），网上很多教程是老的 `i2c_driver_install`，会编不过。

```c
#include "driver/i2c_master.h"

#define I2C_SCL   9
#define I2C_SDA   8
#define INA226_ADDR 0x40

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;

void i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_new_master_bus(&bus_cfg, &bus);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(bus, &dev_cfg, &dev);
}

// 读一个 16 位寄存器（大端）
uint16_t ina226_read(uint8_t reg)
{
    uint8_t buf[2] = {0};
    i2c_master_transmit_receive(dev, &reg, 1, buf, 2, pdMS_TO_TICKS(50));
    return (uint16_t)((buf[0] << 8) | buf[1]);
}
```

**先扫描总线找地址**（不确定器件在不在时用）：

```c
for (uint8_t addr = 1; addr < 127; addr++) {
    if (i2c_master_probe(bus, addr, pdMS_TO_TICKS(20)) == ESP_OK) {
        ESP_LOGI("I2C", "发现设备 0x%02X", addr);
    }
}
```

**和 F407 对照：**

| F407 | ESP32 (v6.1) |
|---|---|
| `I2C_Init()` | `i2c_new_master_bus()` |
| 直接发地址+数据 | 先 `add_device` 再 `transmit` |
| `I2C_ReadData()` | `i2c_master_transmit_receive()` |

**你的项目里**：INA226 挂在电源域（F103），但如果要在 S3 上读传感器，就是这个写法。

---

## 10. TWAI：CAN（你项目里最重要的一节）

**目标**：把 CAN 跑起来。

**名字对照**：ESP32 里 CAN 叫 **TWAI**（Two-Wire Automotive Interface），就是 CAN 2.0B。

```c
#include "driver/twai.h"

#define CAN_TX  GPIO_NUM_4
#define CAN_RX  GPIO_NUM_5

void can_init(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
}
```

**发送一帧：**

```c
twai_message_t msg = {
    .identifier = 0x100,
    .data_length_code = 8,
    .data = {1, 2, 3, 4, 5, 6, 7, 8},
};
twai_transmit(&msg, pdMS_TO_TICKS(100));
```

**接收：**

```c
twai_message_t rx;
if (twai_receive(&rx, pdMS_TO_TICKS(100)) == ESP_OK) {
    ESP_LOGI("CAN", "收到 ID=0x%03X DLC=%d D0=%d",
             rx.identifier, rx.data_length_code, rx.data[0]);
}
```

**⚠️ 没有收发器的现象：**

你现在没接 SN65HVD230，`twai_start()` 会成功、`state=1`（RUNNING），但一旦调 `twai_transmit()`：

- CAN 控制器要等别的节点发 ACK
- 没有收发器 = 总线上没人 = **没人 ACK**
- 重发几次后进入 **BUS_OFF**

**这是正常的**，接了收发器就好了。想在没有硬件时验证收发，可以用 `TWAI_MODE_NO_ACK`（自测模式，自己给自己 ACK）。

**和 F407 对照：**

| F407 | ESP32 |
|---|---|
| `CAN_Init(CAN1, &cfg)` | `twai_driver_install()` |
| `CAN_Transmit()` | `twai_transmit()` |
| `CAN_Receive()` | `twai_receive()` |
| 位定时 `BS1/BS2/Prescaler` | 宏 `TWAI_TIMING_CONFIG_1MBITS()` |
| 过滤器组 | `twai_filter_config_t` |

**注意**：ESP32-S3 **只有 1 路 TWAI，不支持 CAN FD**。你的项目用 CAN 2.0B 标准帧，正好够。

---

## 11. NVS：掉电保存

**目标**：保存参数（比如 PID 的 Kp、标定值）。

```c
#include "nvs_flash.h"
#include "nvs.h"

void nvs_demo(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    nvs_handle_t h;
    nvs_open("calib", NVS_READWRITE, &h);

    // 写
    float kp = 2.5f;
    nvs_set_blob(h, "kp", &kp, sizeof(kp));
    nvs_commit(h);

    // 读
    float kp_out = 0;
    size_t len = sizeof(kp_out);
    if (nvs_get_blob(h, "kp", &kp_out, &len) == ESP_OK) {
        ESP_LOGI("NVS", "kp = %.2f", kp_out);
    }
    nvs_close(h);
}
```

**和 F407 对照：**

| F407 | ESP32 |
|---|---|
| 自己算 Flash 扇区、擦除、写 | NVS 全包了 |
| 掉电写一半会坏 | NVS 有日志结构和磨损均衡 |
| 只能存 Flash 页对齐 | 任意 key-value |

**你的项目里**：电源域的校准值、PID 参数、故障码，都该进 NVS。

---

## 12. 双核：把任务绑到指定核

**目标**：理解"为什么要绑核"。

```c
// 绑到核 1（APP_CPU）
xTaskCreatePinnedToCore(my_task, "t1", 4096, NULL, 5, NULL, 1);

// 绑到核 0（PRO_CPU，WiFi 协议栈在这）
xTaskCreatePinnedToCore(wifi_like_task, "t0", 4096, NULL, 5, NULL, 0);
```

**查看当前任务跑在哪个核：**

```c
ESP_LOGI("CORE", "当前在核 %d", xPortGetCoreID());
```

**你项目的分工（已定）：**

| 核 0 | 核 1 |
|---|---|
| WiFi 协议栈 | CAN 收发 |
| 摄像头采集 + JPEG | 状态机 |
| LoRa | 心跳超时 |
| PSRAM 日志 | — |

**原因**：WiFi 协议栈会随机占用 CPU 几百微秒，如果和 CAN 收发同核，CAN 的实时性会被破坏。

---

## 13. 常见坑（都在你项目里遇到过或会遇到）

| 坑 | 现象 | 解法 |
|---|---|---|
| `while(1)` 里没 `vTaskDelay` | 看门狗复位 / 别的任务不跑 | 加 `vTaskDelay` |
| 任务栈太小 | `***ERROR*** A stack overflow in task xxx` | 加大到 8192 |
| ISR 里用非 FromISR API | 崩溃 / 断言失败 | 换 `xQueueSendFromISR` |
| ISR 没加 `IRAM_ATTR` | Flash 操作时随机崩 | 加 `IRAM_ATTR` |
| 用了 GPIO43/44 | 日志乱 / 功能异常 | 这两个是串口，避开 |
| 忘记 `REQUIRES` | 链接报 `undefined reference` | CMakeLists 加组件 |
| 老版 I2C API | 编译报 `i2c_driver_install` 不存在 | 用 `i2c_new_master_bus` |
| `-Og` 编译崩溃 | `internal compiler error: Segmentation fault` | 改 `CONFIG_COMPILER_OPTIMIZATION_PERF=y` |

---

## 14. 学习路线（对应你的项目）

按这个顺序学，每一节学完都能往项目里挪一点：

| 阶段 | 学什么 | 对应项目模块 | 时间 |
|---|---|---|---|
| 1 | §2 日志 + §3 GPIO | 点灯、看日志 | 半天 |
| 2 | §4 任务 + §5 队列 | 所有模块的骨架 | 1 天 |
| 3 | §10 TWAI | **CAN 网关（核心）** | 1 天 |
| 4 | §8 UART | **LoRa 车际链路** | 半天 |
| 5 | §6 定时器 + §7 中断 | 心跳、超时检测 | 1 天 |
| 6 | §11 NVS | 参数保存 | 半天 |
| 7 | §12 双核 | 性能优化（最后做） | 半天 |
| 8 | WiFi AP + WebSocket | 手机控制 | 2 天 |
| 9 | 摄像头（esp32-camera） | 图像链路 | 2 天 |

**别跳着学**：§4、§5 是地基，所有后面代码都建立在"任务 + 队列"上。

---

## 附：怎么新建一个练习工程

```bash
# 1. 复制骨架
xcopy /E /I 00_s3_bringup 01_s3_practice

# 2. 删掉旧 build
rmdir /S /Q 01_s3_practice\build

# 3. 改 sdkconfig.defaults 里的 project 名（可选）

# 4. 编译烧录（改路径即可）
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\01_s3_practice" build
python D:\GitHub_Project\_build_bringup.py "D:\GitHub_Project\CAR_Project\firmware\01_s3_practice" flash
```

**每次只改 `main/main.c` 一个文件**，改完就编译烧录，看现象。

---

## 一句话总结

> **把 ESP32 当成"带 WiFi 的 F407 + 强制用 RTOS"来学。**
> 外设怎么想你还用 F407 的经验；真正要新学的是**任务、队列、组件化构建**这三样。
