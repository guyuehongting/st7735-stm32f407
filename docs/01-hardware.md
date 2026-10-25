# 01 · 硬件与引脚约束

## 硬件清单

| 项目 | 型号 | 关键参数 |
|---|---|---|
| MCU | STM32F407VET6 | Cortex-M4F，168MHz，512KB Flash，192KB RAM，LQFP100 |
| 屏 | ST7735S 1.8" TFT | 可见区 128×160，控制器显存 132×162，RGB565 |
| 模块 | 8 脚 SPI 模块 | VCC / GND / SCL / SDA / RES / DC / CS / BLK |
| 库 | STM32 HAL | CubeMX 生成基础工程 |

**STM32F407VET6 是 LQFP100 封装，没有 Port F 和 Port G。** 这一点影响很大 —— 很多例程默认用 PF/PG，直接编译就报错。

---

## 时钟

```
HSI 16MHz
  → PLLM=16 / PLLN=336 / PLLP=2 / PLLQ=7
  → SYSCLK 168MHz
      AHB  = 168MHz  (div 1)
      APB1 =  42MHz  (div 4)   ← SPI2 挂这里
      APB2 =  84MHz  (div 2)   ← SPI1 / USART1 挂这里
```

### 用 HAL_RCC_ClockConfig，不要直接写寄存器

如果像很多例程那样直接写 `RCC->PLLCFGR` / `RCC->CFGR` 切到 168MHz，
`HAL_Delay()` 会**快 10.5 倍**（SysTick 还是按 16MHz 算的），
所有延时都失效 —— 复位时序、初始化等待全部不对。

```c
RCC_OscInitTypeDef osc = {0};
RCC_ClkInitTypeDef clk = {0};

osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
osc.HSIState            = RCC_HSI_ON;
osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
osc.PLL.PLLState        = RCC_PLL_ON;
osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
osc.PLL.PLLM            = 16;
osc.PLL.PLLN            = 336;
osc.PLL.PLLP            = RCC_PLLP_DIV2;
osc.PLL.PLLQ            = 7;
HAL_RCC_OscConfig(&osc);

clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
clk.APB1CLKDivider = RCC_HCLK_DIV4;
clk.APB2CLKDivider = RCC_HCLK_DIV2;
HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5);   /* 168MHz 必须 5 个等待周期 */
```

`HAL_RCC_ClockConfig()` 内部会重新调用 `HAL_InitTick()`，SysTick 自动按新频率重算，
`HAL_Delay()` 才是准的。

---

## SPI2 复用表（关键）

**SCL 物理上接在 PB10，而 PB10 在全芯片上只能做 SPI2_SCK。**
所以整条链路必须是 SPI2，用 SPI1 的代码去驱动 PB10 是完全无效的。

### STM32F407 SPI2 引脚复用（AF5）

| 信号 | 可选引脚 |
|---|---|
| **SCK** | **PB10** / PB13 / PD3 |
| **MISO** | PB14 / PC2 |
| **MOSI** | **PC3** / PB15 |

**注意 MOSI 全芯片只有 PC3 和 PB15 两个选择。** 这是排除法的基础 ——
只要确认了 SCL 是 PB10，SDA 就必然在这两个里。

本项目的选择：

```
SCK  = PB10   (AF5)
MOSI = PC3    (AF5)
```

### 寄存器配置

```c
/* PB10 = SPI2_SCK，AFR[1] bits[11:8] */
GPIOB->MODER   &= ~(3u << 20);  GPIOB->MODER   |= (2u << 20);
GPIOB->OSPEEDR |=  (3u << 20);
GPIOB->AFR[1]  &= ~(0xFu << 8); GPIOB->AFR[1]  |= (5u << 8);

/* PC3 = SPI2_MOSI，AFR[0] bits[15:12] */
GPIOC->MODER   &= ~(3u << 6);   GPIOC->MODER   |= (2u << 6);
GPIOC->OSPEEDR |=  (3u << 6);
GPIOC->AFR[0]  &= ~(0xFu << 12); GPIOC->AFR[0] |= (5u << 12);

/* Mode 0：CPOL=0 CPHA=0，8 位，MSB 先出 */
SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | BR_BITS | SPI_CR1_SPE;
```

### 为什么用寄存器而不是 HAL_SPI

CubeMX 生成的工程里 `stm32f4xx_hal_conf.h` 的 `HAL_SPI_MODULE_ENABLED` 可能被注释掉，
此时编译会报：

```
error: #20: identifier "SPI_HandleTypeDef" is undefined
error: #20: identifier "SPI_TypeDef" is undefined   (x10)
```

判断方法：看编译日志里有没有 `hal_spi.c`。有 `hal_uart.c`、`hal_gpio.c` 却没有 `hal_spi.c`
= 这个模块没开。

寄存器版不依赖任何 HAL 模块开关，更省事。

### SPI 速度分频

APB1 = 42MHz：

| BR[2:0] | 分频 | 频率 | 宏 |
|---|---|---|---|
| 001 | /4 | 10.5 MHz | `SPI_CR1_BR_0` |
| 010 | /8 | 5.25 MHz | `SPI_CR1_BR_1` |
| 011 | /16 | 2.6 MHz | `SPI_CR1_BR_1 \| SPI_CR1_BR_0` |
| 100 | /32 | 1.3 MHz | `SPI_CR1_BR_2` |
| 101 | /64 | **656 kHz** | `SPI_CR1_BR_2 \| SPI_CR1_BR_0` |
| 110 | /128 | 328 kHz | `SPI_CR1_BR_2 \| SPI_CR1_BR_1` |

本项目默认 `/64 = 656kHz`。实测稳定；更高也可以试，但先确保能显示。

---

## 引脚占用与约束

### 已经占用的引脚（不能碰）

| 引脚 | 用途 |
|---|---|
| PA13 / PA14 | SWD 调试口，**绝对不能占用** |
| PA9 / PA10 | USART1（调试串口） |
| PA0 | TIM2_CH1（PWM） |
| PA2~PA5 | ADC1 |
| PA6 / PA7 | TIM3 编码器 |
| PA11 / PA12 | USB |
| PB6 | LED1 |
| PB12 / PB13 | 电机驱动 |
| PE2~PE5 | 按键 |

### PC13 / PC14 / PC15 在备份域（重要）

这三个脚属于**备份域（Backup Domain）**，有硬件限制：

- 输出电流只有 **3mA**（普通 IO 是 25mA）
- 翻转速度上不去
- 不能当电流源（不能直接驱动 LED）

本项目最终 CS/DC 就落在 PC14/PC15 上，所以 SPI 时钟必须降到 656kHz。
这是实测出来的限制，不是猜的。

**如果重新设计硬件，建议把 CS/DC 放在普通 IO 上**（比如 PB0/PB1），
这样 SPI 可以跑 5~10MHz，全屏刷新从 0.5 秒降到 0.06 秒。

---

## 模块丝印的坑

1.8 寸 8 脚 ST7735 模块至少有三种排法，常见的是：

```
GND  VCC  SCL  SDA  RES  DC  CS  BLK     ← 最常见
VCC  GND  SCL  SDA  RES  DC  CS  BLK
BLK  CS   DC   RES  SDA  SCL  GND VCC    ← 也有反着排的
```

**只信你手上那块板的丝印。** 丝印缩写对照：

| 丝印 | 含义 |
|---|---|
| SCL / SCK / CLK | SPI 时钟 |
| SDA / MOSI / DIN | SPI 数据（单片机 → 屏） |
| CS / CE / SS | 片选，低有效 |
| DC / A0 / RS | 命令/数据选择：低=命令，高=数据 |
| RES / RST | 复位，低有效 |
| BLK / LED / BL / LEDA | 背光正极 |

本项目实测：**用户最初把 CS/DC 看成了 BLK**，导致"拉低找 BLK"整轮白做。
如果丝印被排针挡住，宁可从电路上反推（见 [04-methodology.md](04-methodology.md)），
也不要凭猜测。

---

## 供电

| 项目 | 值 | 说明 |
|---|---|---|
| VCC | 3.3V | ST7735 逻辑不耐 5V |
| BLK | 3.3V 起 | 有的模块自带限流电阻可直接 5V，有的不行 |
| 电流 | 约 20~40mA | 背光占大头 |

`RCC->CSR` 的复位标志可以帮上忙：

```c
RCC->CSR = 0x04000000;   /* PIN = 引脚复位，正常上电 */
/* 如果出现 BOR 标志，说明供电不足/跌落 */
```

本项目实测 `0x04000000 (PIN)`，无 BOR，所以供电方向从一开始就可以排除。
