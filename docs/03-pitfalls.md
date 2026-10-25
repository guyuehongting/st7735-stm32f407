# 03 · 三个真正的根因（代码级分析）

> 引脚一直是对的。真正让屏幕不出画面的，是这三处。
>
> 1. 发像素前没拉高 DC（判据全废）
> 2. CS/DC 接反（命令通、数据断）
> 3. RES 悬空（面板跑几秒自己复位，变白屏）

---

## 根因 1：发像素前没有把 DC 拉高

### 现象

- 屏幕能花屏，说明字节到面板了
- 但**任何填色都不生效**，屏幕永远只有"白"和"花"两种状态
- 换什么颜色、什么坐标都一样

### 出问题的代码

```c
static void TFT_Cmd(uint8_t cmd)
{
    HAL_GPIO_WritePin(s_dc.port, s_dc.pin, GPIO_PIN_RESET);   /* DC = 低 */
    SPI2_Send(&cmd, 1);
}

static void TFT_Data(const uint8_t *buf, uint16_t len)
{
    HAL_GPIO_WritePin(s_dc.port, s_dc.pin, GPIO_PIN_SET);     /* DC = 高 */
    SPI2_Send(buf, len);
}

static void TFT_WindowFull(void)
{
    ...
    TFT_Cmd(0x2C);        /* ← 最后一句是 TFT_Cmd，DC 停在低电平 */
}

static void TFT_FillAll(uint16_t color)
{
    ...
    TFT_WindowFull();                    /* 返回时 DC 还是低 */
    for (h = 0; h < GRAM_H; h++)
    {
        SPI2_Send(s_line, GRAM_W * 2);   /* ← 直接调底层，没拉高 DC */
    }
}
```

### 为什么错

`0x2C`（RAMWR）是"开始写显存"命令。**它之后的每一个字节都是像素数据**，
必须用 DC=高 发送。

但 `TFT_Cmd()` 的职责是"拉低 DC 再发一个字节"，它**发完不会恢复 DC**。
于是 `TFT_WindowFull()` 一返回，DC 就停在低电平，
紧接着的 42,768 个像素字节全部被面板当成**命令**解释。

### 后果链

```
像素数据被当命令
  → 面板执行了一堆随机"命令"
  → 显存里是随机内容 / 窗口状态被改乱
  → 屏幕显示花屏，或者干脆没反应
  → 看起来像"引脚不对"，实际上是判据坏了
```

### 正确写法

```c
TFT_WindowFull();          /* 内部以 TFT_Cmd(0x2C) 结束，DC = 低 */
DC_HIGH();                 /* ★ 必须补这一行 */
for (h = 0; h < GRAM_H; h++)
{
    SPI_Send(s_line, (uint16_t)(GRAM_W * 2));
}
```

### 更稳的写法

把"写像素"封装成一个函数，让调用者不可能忘：

```c
/* 打开一个写入窗口，之后所有 SPI_Send 都是像素数据 */
static void TFT_BeginWrite(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];

    TFT_Cmd(0x2A);
    d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)(x0 & 0xFF);
    d[2] = (uint8_t)(x1 >> 8); d[3] = (uint8_t)(x1 & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2B);
    d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)(y0 & 0xFF);
    d[2] = (uint8_t)(y1 >> 8); d[3] = (uint8_t)(y1 & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2C);
    DC_HIGH();             /* ★ 封装在内部，调用者不会漏 */
}
```

本仓库 `driver/main.c` 里的 `TFT_SetWindow()` 就是这个思路，
但为了让你看清楚坑在哪，`TFT_ClearAll()` 和 `TFT_FillRect()` 里仍然显式写了 `DC_HIGH()`。

---

## 根因 2：CS 和 DC 接反了

### 现象

**命令全通，数据全断。**

- `SWRESET`、`SLPOUT`、`DISPON`、`INVON`、`INVOFF` 全部生效（屏幕会闪）
- 但所有带参数的命令（`0x2A/0x2B` 窗口、`0x36` 方向、`0x3A` 色深）和所有像素数据全部丢失
- 填色"一点变化都没有"

### 为什么"命令通、数据断"只有一种可能

每个字节都要 DC 配合：

```
命令字节：DC = 低
数据字节：DC = 高
```

现在**低电平有效、高电平无效**，说明我们翻转的那根线不是 DC，而是 CS：

```
我们以为：PC14 = CS（恒定拉低），PC15 = DC（翻转）
实际情况：PC14 = DC（被恒定为低 → 永远命令模式 → 命令全进 ✓）
          PC15 = CS（在跳）
             · 发命令时 PC15 恰好被拉低 → 片选有效 → 命令进得去 ✓
             · 发数据时 PC15 被拉高   → 片选失效 → 数据全丢 ✗
```

### 为什么填色"完全不变"而不只是"颜色错"

`0x2A / 0x2B` 的参数也是数据，也丢了。所以窗口地址停在默认的 `(0,0)-(0,0)`，
**只有 1 个像素**。写 4 万个像素进去，只有第一个落在这 1 个像素的窗口里，
整屏看起来完全没动。

### 一个反直觉的事实：两种接法都会"闪"

`0x21 INVON` 是**纯命令、没有参数**，所以两种接法都能把它送进去：

| 组合 | 真 DC 的行为 | 真 CS 的行为 | INVON 能进吗 |
|---|---|---|---|
| `CS=PC14 / DC=PC15` | PC14 恒低 → 永远命令模式 | PC15 在跳 → 命令期恰好低 | **能** ✓ |
| `CS=PC15 / DC=PC14` | PC14 在跳 → 正常 | PC15 恒低 → 一直选中 | **能** ✓ |

**这就是为什么用 INVON 扫描时两组都会变，看起来"分不出对错"。**

只有一组是两路全对的。另一组是靠"命令期 CS 恰好低"蒙进去的。

### 怎么快速区分

用**带参数的命令**或**写像素**，因为这两类动作依赖 DC=高：

```c
/* 判据：这个组合能不能让屏幕变成纯色 */
TFT_Cmd(0x2A); TFT_Data(caset, 4);     /* 依赖 DC=高 */
TFT_Cmd(0x2B); TFT_Data(raset, 4);     /* 依赖 DC=高 */
TFT_Cmd(0x2C);
DC_HIGH();
for (...) SPI_Send(pixels);            /* 依赖 DC=高 */
```

能出纯色 = 两路全对。

### 最省事的验证方法：两套顺序一次跑完

```c
/* PHASE A：CS=PC15  DC=PC14 */
/* PHASE B：CS=PC14  DC=PC15 */
```

各填红绿蓝白 2 秒。哪一半出纯色，哪一半就是对的。
本项目实测 **PHASE A 通过**。

---

## 顺带：CS 可以一直拉低

总线上只有一个 SPI 设备时，**CS 完全可以常低**，不需要每次传输都翻转。

这样反而更安全 —— 少一个时序变量。本项目的做法：

```c
CS_LOW();          /* 初始化一开始就拉低，之后再也不动 */
TFT_Cmd(0x01);     /* TFT_Cmd / TFT_Data 都不碰 CS */
```

代价是：以后如果要挂第二个 SPI 设备，必须改回"每次传输都控制 CS"。

---

## 两个坑的排查顺序（推荐）

如果屏幕不出画面，按这个顺序查，别跳步：

```
1. 纯白？
   ├─ 是 → SCK / SDA / CS 至少错一个
   │       查：SPI 总线是否选对（PB10 只能 SPI2）
   │       查：MOSI 是否在 PC3 / PB15 里
   └─ 否 → 花屏，进入 2

2. 花屏 → 字节到了，DC 有问题
   ├─ 带参数的命令生效吗？（比如改 MADCTL 后画面翻转）
   │   ├─ 生效 → DC 对了，查窗口 / COLMOD / 偏移
   │   └─ 不生效 → 进入 3
   └─

3. 命令生效但数据不生效 → 两个可能
   ├─ 发像素前没拉高 DC        ← 查代码
   └─ CS/DC 接反了             ← 两套顺序各跑一遍
```

这三步能覆盖 95% 的白屏/花屏问题。


---

## 根因 3：RES 悬空，面板跑几秒就自己复位

### 现象

- 上电后**红、绿、蓝、白闪了几下**（说明初始化成功、像素通路正常）
- 然后**变成一片纯白，再也不变**
- 按 MCU 复位也没用（或者闪几下又白）
- 串口**还在正常打印**（说明 MCU 没死，还在写像素）

### 为什么是"纯白"而不是"停在上一个画面"

ST7735 是**常白**屏。面板一旦被复位，显示关闭、液晶不加电 → 全白。

而我们的驱动**只在开机时初始化一次**。面板复位之后再写像素，写进去的是
一个"没初始化"的控制器 —— 显示没打开，所以什么都看不见，永远白。

### 根因：RES 那根线悬空

本项目 RES 不接（靠 `0x01 SWRESET` 软复位）。但**不接 ≠ 可以不驱动**。

RES 悬空时，它的电平由漏电流和噪声决定。跑几秒钟，一个干扰就能把它拉低一次 ——
面板立刻硬复位，回到白屏。

### 为什么测试程序从来没这个问题

测试程序每一组前都调 `Pins_HighExcept()`，把所有候选脚都拉成**输出高电平**：

```c
static void Pins_HighExcept(const PinDef *exclude)
{
    ...
    for (i = 0; i < POOL_N; i++)
    {
        gi.Pin = s_pool[i].pin;
        HAL_GPIO_Init(s_pool[i].port, &gi);
        HAL_GPIO_WritePin(s_pool[i].port, s_pool[i].pin, GPIO_PIN_SET);   /* ← 全拉高 */
    }
}
```

RES 不管是哪一根，都被**钉在高电平**，所以面板永远不会自己复位。

正式驱动里只驱动了 CS / DC，把这一步丢了 —— 这就是差别。

### 修法：把所有"没用到但可能是 RES / BLK"的脚全部拉高

```c
static void TFT_ParkPinsHigh(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    gi.Mode  = GPIO_MODE_INPUT;      /* ★ 输入 + 上拉，不是推挽输出 */
    gi.Pull  = GPIO_PULLUP;
    gi.Speed = GPIO_SPEED_FREQ_LOW;

    /* GPIOE: PE2 PE3 PE4 PE5 PE6 */
    gi.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOE, &gi);

    /* GPIOC: PC0 PC1 PC2 PC13（PC3=MOSI / PC14=DC / PC15=CS 不能用） */
    gi.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOC, &gi);

    /* GPIOB: PB13 PB15（PB10=SCK 不能用） */
    gi.Pin = GPIO_PIN_13 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &gi);
}
```

在 `TFT_Init()` 里、`SPI2_Init()` 之前调用一次。

#### 为什么用「输入 + 上拉」而不是「推挽输出高」

推挽输出高虽然更"硬"，但这几根脚在你的板子上另有用途：

| 引脚 | 板上的用途 | 推挽输出高的后果 |
|---|---|---|
| PE2~PE5 | 按键 | 按键按下时 = 对地短路，瞬间大电流 |
| PB13 | 电机方向 | 可能把电机驱动打开，电流冲击 + 干扰 |

输入上拉只有几十微安，既能给高阻的 RES 输入一个确定的高电平，
又绝不会短路、也不会把电机打开。对 RES 这种高阻输入来说完全够用。

**实测结论：换成输入上拉后，屏幕一路正常计数到 300+，问题解决。**

### 第二道保险：每段演示前重新初始化

即使 RES 又被噪声拉低了，下一段演示开始时重新 init 一次就能救回来，
不会出现"卡在白屏再也回不来"。

```c
while (1)
{
    TFT_Init();  Demo_Colors();
    TFT_Init();  Demo_Bars();
    ...
}
```

成本只有 0.4 秒一次，换来的是"永不卡死"。

### 最彻底的修法：把 RES 接出来

如果你的模块 RES 有引出，**接一个 GPIO，初始化时拉低 20ms 再拉高**，
比软复位可靠得多：

```c
RES_LOW();
HAL_Delay(20);
RES_HIGH();
HAL_Delay(120);
/* 然后才开始发命令 */
```

### 怎么判断是不是这个原因

看串口：

| 串口 | 屏幕 | 结论 |
|---|---|---|
| **还在打印** | 白 | **面板被复位了**（MCU 活着，就是本节的毛病） |
| 停止打印 | 停在某一色 | MCU 卡死或 HardFault |
| 一直打印 | 一直正常 | 没问题 |

串口还在打点而屏幕是白的，基本就是 RES / 供电的事。
