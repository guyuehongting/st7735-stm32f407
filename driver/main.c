/*==============================================================================
 *  ST7735S  1.8"  128x160  TFT  驱动 + 演示程序
 *  目标：STM32F407VET6  /  HAL 库  /  Keil MDK
 *
 *  ---------------------------------------------------------------------------
 *  接线（2026-10-25 实测通过，别再改）
 *  ---------------------------------------------------------------------------
 *      模块丝印          接到         说明
 *      VCC              3.3V
 *      GND              GND
 *      SCL / SCK        PB10         SPI2_SCK   (AF5)
 *      SDA / MOSI       PC3          SPI2_MOSI  (AF5)
 *      CS  / CE         PC15         片选（低有效，程序里一直拉低）
 *      DC  / A0 / RS    PC14         命令=低 / 数据=高
 *      RES             不接          用软件复位 0x01 SWRESET
 *      BLK / LED        3.3V          背光（想调亮度就接 PB8 做 PWM）
 *
 *  ---------------------------------------------------------------------------
 *  为什么 CS/DC 是这两个脚（踩过的坑，记下来免得以后又反）
 *  ---------------------------------------------------------------------------
 *      PC14 恒定拉低时命令能进、数据进不去 —— 说明被当 DC 翻转的那根其实是 CS。
 *      正确组合是：CS = PC15（常低），DC = PC14（翻转）。
 *      另外一个必踩的坑：TFT_Cmd() 会把 DC 拉低且不恢复，
 *      所以「发像素」之前必须自己把 DC 拉高，否则所有像素字节都被当命令解释，
 *      屏幕永远只有花屏、出不了纯色。
 *
 *  ---------------------------------------------------------------------------
 *  用法
 *  ---------------------------------------------------------------------------
 *      把本文件整个覆盖到工程里的 Core/Src/main.c，编译烧录即可。
 *      不需要改 Keil 分组，不需要加文件，不需要动 CubeMX。
 *
 *  ---------------------------------------------------------------------------
 *  第一次跑起来之后，按现象调这三个开关
 *  ---------------------------------------------------------------------------
 *      画面上下/左右颠倒      -> MADCTL_VAL 换一个（0x00 / 0x60 / 0xA0 / 0xC0）
 *      红蓝互换               -> USE_BGR 改 1
 *      边缘少几列/多几列杂色  -> X_OFF / Y_OFF 试 2 / 1
 *      想跑快一点             -> SPI_BR 改 2（1.3MHz）或 1（2.6MHz）
 *============================================================================*/

#include "stm32f4xx_hal.h"

/*==============================================================================
 *                              开关区
 *============================================================================*/

/* SPI2 分频：APB1 = 42MHz
 *   0 = /8    -> 5.25 MHz
 *   1 = /16   -> 2.6  MHz
 *   2 = /32   -> 1.3  MHz
 *   3 = /64   -> 656  kHz   <- 默认，最稳
 *   4 = /128  -> 328  kHz
 *   5 = /256  -> 164  kHz                                              */
#define SPI_BR          3

/* MADCTL（0x36）取值，决定方向和镜像
 *   0xC0 = 上下左右都翻（本项目实测通过的默认值，先用它）
 *   0x00 = 正常          0x60 = 上下翻          0xA0 = 左右翻            */
#define MADCTL_VAL      0xC0

/* 红蓝通道互换：屏幕红色显示成蓝色时改成 1 */
#define USE_BGR         0

/* 行列偏移：1.8" 模块的可见区通常是 132x162 显存里的 128x160
 * 画面整体偏左上/右下、边缘有杂色时，试 X_OFF=2 / Y_OFF=1               */
#define X_OFF           0
#define Y_OFF           0

/* 伽马校正：颜色发白、发暗、过渡不自然时改成 1 */
#define USE_GAMMA       0

/* 串口进度打印（USART1，PA9=TX PA10=RX，115200）。不需要就改 0 */
#define USE_SERIAL      1

/* 诊断模式：
 *   1 = 跑「静态保持」诊断（排查"跑几秒变白"用，会占满整个程序）
 *   0 = 跑正常的 7 段演示                                    <- 默认
 *   2026-10-25 已实测通过：屏幕能一路计数到 300+，说明 RES 问题已解决。  */
#define RUN_DIAG        0

/*==============================================================================
 *                              尺寸 / 颜色
 *============================================================================*/

#define GRAM_W          132         /* 控制器显存宽（ST7735 都是 132） */
#define GRAM_H          162         /* 控制器显存高 */
#define LCD_W           128         /* 可见区宽 */
#define LCD_H           160         /* 可见区高 */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

#define C_BLACK         0x0000
#define C_WHITE         0xFFFF
#define C_RED           0xF800
#define C_GREEN         0x07E0
#define C_BLUE          0x001F
#define C_YELLOW        0xFFE0
#define C_CYAN          0x07FF
#define C_MAGENTA       0xF81F
#define C_GRAY          0x8410
#define C_ORANGE        0xFD20

/*==============================================================================
 *                              引脚定义
 *============================================================================*/

#define PIN_CS_PORT     GPIOC
#define PIN_CS_PIN      GPIO_PIN_15
#define PIN_DC_PORT     GPIOC
#define PIN_DC_PIN      GPIO_PIN_14

#define CS_LOW()        (PIN_CS_PORT->BSRR = ((uint32_t)PIN_CS_PIN << 16))
#define CS_HIGH()       (PIN_CS_PORT->BSRR =  (uint32_t)PIN_CS_PIN)
#define DC_LOW()        (PIN_DC_PORT->BSRR = ((uint32_t)PIN_DC_PIN << 16))
#define DC_HIGH()       (PIN_DC_PORT->BSRR =  (uint32_t)PIN_DC_PIN)

/*==============================================================================
 *                         一、时钟（HSI 16M -> 168MHz）
 *   用 HAL_RCC_ClockConfig 而不是直接写寄存器，
 *   这样 HAL 会顺便重算 SysTick，HAL_Delay 才是准的。
 *============================================================================*/
static void SystemClock_Config(void)
{
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
    (void)HAL_RCC_OscConfig(&osc);

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5);
}

/*==============================================================================
 *                         二、串口（纯寄存器，115200）
 *============================================================================*/
#if USE_SERIAL

static void USART1_InitRaw(uint32_t pclk2, uint32_t baud)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIOA->MODER   &= ~((3u << 18) | (3u << 20));
    GPIOA->MODER   |=  ((2u << 18) | (2u << 20));
    GPIOA->OSPEEDR |=  ((3u << 18) | (3u << 20));
    GPIOA->PUPDR   &= ~((3u << 18) | (3u << 20));
    GPIOA->AFR[1]  &= ~((0xFu << 4) | (0xFu << 8));
    GPIOA->AFR[1]  |=  ((7u << 4) | (7u << 8));

    USART1->CR1 = 0;
    USART1->BRR = (pclk2 + (baud / 2u)) / baud;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void U_Send(const char *s)
{
    while (*s != 0)
    {
        while ((USART1->SR & USART_SR_TXE) == 0)
        {
        }
        USART1->DR = (uint8_t)(*s);
        s++;
    }
}

static void U_Num(uint32_t v)
{
    char b[11];
    int8_t i = 10;

    b[10] = 0;
    if (v == 0)
    {
        U_Send("0");
        return;
    }
    while (v > 0 && i > 0)
    {
        i--;
        b[i] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    U_Send(&b[i]);
}

#define U_LOG(x)        U_Send(x)
#define U_LOG_N(x)      U_Num((uint32_t)(x))

#else

#define U_LOG(x)
#define U_LOG_N(x)

#endif

/*==============================================================================
 *                         三、SPI2（纯寄存器，AF5）
 *============================================================================*/
#if   (SPI_BR == 0)
#define BR_BITS         SPI_CR1_BR_1
#elif (SPI_BR == 1)
#define BR_BITS         (SPI_CR1_BR_1 | SPI_CR1_BR_0)
#elif (SPI_BR == 2)
#define BR_BITS         SPI_CR1_BR_2
#elif (SPI_BR == 3)
#define BR_BITS         (SPI_CR1_BR_2 | SPI_CR1_BR_0)
#elif (SPI_BR == 4)
#define BR_BITS         (SPI_CR1_BR_2 | SPI_CR1_BR_1)
#else
#define BR_BITS         (SPI_CR1_BR_2 | SPI_CR1_BR_1 | SPI_CR1_BR_0)
#endif

static void SPI2_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    /* PB10 = SPI2_SCK  (AF5)，AFR[1] bits[11:8] */
    GPIOB->MODER   &= ~(3u << 20);
    GPIOB->MODER   |=  (2u << 20);
    GPIOB->OSPEEDR |=  (3u << 20);
    GPIOB->PUPDR   &= ~(3u << 20);
    GPIOB->AFR[1]  &= ~(0xFu << 8);
    GPIOB->AFR[1]  |=  (5u << 8);

    /* PC3 = SPI2_MOSI (AF5)，AFR[0] bits[15:12] */
    GPIOC->MODER   &= ~(3u << 6);
    GPIOC->MODER   |=  (2u << 6);
    GPIOC->OSPEEDR |=  (3u << 6);
    GPIOC->PUPDR   &= ~(3u << 6);
    GPIOC->AFR[0]  &= ~(0xFu << 12);
    GPIOC->AFR[0]  |=  (5u << 12);

    /* Mode 0：CPOL=0 CPHA=0，8 位，MSB 先出 */
    SPI2->CR1 = 0;
    SPI2->CR2 = 0;
    SPI2->I2SCFGR &= (uint16_t)~SPI_I2SCFGR_I2SMOD;
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI
              | BR_BITS | SPI_CR1_SPE;
}

static void SPI_Send(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++)
    {
        while ((SPI2->SR & SPI_SR_TXE) == 0)
        {
        }
        *(__IO uint8_t *)&SPI2->DR = buf[i];

        while ((SPI2->SR & SPI_SR_RXNE) == 0)
        {
        }
        (void)*(__IO uint8_t *)&SPI2->DR;
    }

    while ((SPI2->SR & SPI_SR_BSY) != 0)
    {
    }
}

/*==============================================================================
 *                         四、ST7735 底层
 *============================================================================*/

static uint8_t s_line[GRAM_W * 2];

/* 发命令：DC 拉低。★ 注意：本函数结束后 DC 停在「低」，调用者若要发数据必须自己拉高 */
static void TFT_Cmd(uint8_t cmd)
{
    DC_LOW();
    SPI_Send(&cmd, 1);
}

static void TFT_Data(const uint8_t *buf, uint16_t len)
{
    DC_HIGH();
    SPI_Send(buf, len);
}

static void TFT_Data1(uint8_t v)
{
    TFT_Data(&v, 1);
}

/* 设窗口（逻辑坐标，会自动加 X_OFF / Y_OFF） */
static void TFT_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t d[4];

    x0 = (uint16_t)(x0 + X_OFF);
    x1 = (uint16_t)(x1 + X_OFF);
    y0 = (uint16_t)(y0 + Y_OFF);
    y1 = (uint16_t)(y1 + Y_OFF);

    TFT_Cmd(0x2A);
    d[0] = (uint8_t)(x0 >> 8); d[1] = (uint8_t)(x0 & 0xFF);
    d[2] = (uint8_t)(x1 >> 8); d[3] = (uint8_t)(x1 & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2B);
    d[0] = (uint8_t)(y0 >> 8); d[1] = (uint8_t)(y0 & 0xFF);
    d[2] = (uint8_t)(y1 >> 8); d[3] = (uint8_t)(y1 & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2C);
}

/*==============================================================================
 *                         五、初始化
 *============================================================================*/

/*------------------------------------------------------------------------------
 * ★★ 关键：把所有「没用到但可能是 RES / BLK」的脚全部拉高
 *
 *   为什么必须有这一步：
 *     模块的 RES 我们没接（靠 0x01 SWRESET 软复位）。
 *     如果 RES 那根线悬空，跑几秒钟就会被噪声拉低一次 ——
 *     面板一复位就回到出厂的白屏（显示关闭），
 *     之后所有写像素都写进一个「没初始化」的面板，屏幕就永远白着。
 *
 *     测试程序里之所以没这个问题，是因为每组测试前 Pins_HighExcept()
 *     把所有候选脚都拉高了，RES 一直被钉在高电平。
 *     正式驱动里如果只驱动 CS/DC，就漏掉了这件事。
 *
 *   顺带的好处：BLK（背光）如果在这几个脚里，也会被拉高 -> 背光常亮。
 *---------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
 * ★★ 把所有「没用到但可能是 RES / BLK」的脚都拉到高电平
 *
 *   为什么需要：
 *     模块的 RES 我们没接（靠 0x01 SWRESET 软复位）。RES 悬空时电平由噪声决定，
 *     被拉低一次面板就硬复位 -> 显示关闭 -> 常白屏 -> 之后写什么都没用。
 *
 *   为什么用「输入 + 上拉」而不是「推挽输出高」：
 *     · PE2~PE5 是按键，如果按键正好被按住，推挽输出高 = 对地短路，电流很大
 *     · PB13 是电机方向脚，推挽输出高可能把电机驱动打开，电流冲击 + 干扰
 *     输入上拉只有几十微安，既能给 RES 一个确定的高电平，又绝不会短路、
 *     也不会把电机打开。对高阻的 RES 输入来说，这个上拉完全够用。
 *---------------------------------------------------------------------------*/
static void TFT_ParkPinsHigh(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    gi.Mode  = GPIO_MODE_INPUT;
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

static void TFT_Init(void)
{
    GPIO_InitTypeDef gi = {0};
    uint8_t d3[3];
    uint8_t d2[2];

    __HAL_RCC_GPIOC_CLK_ENABLE();       /* PC14/PC15 的时钟必须先开，再配置 */

    /* CS / DC 都是推挽输出 */
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Pin   = PIN_CS_PIN | PIN_DC_PIN;
    HAL_GPIO_Init(GPIOC, &gi);

    CS_HIGH();
    DC_HIGH();
    HAL_Delay(20);

    TFT_ParkPinsHigh();             /* ★ RES 必须被钉在高电平，否则面板会自己复位 */

    SPI2_Init();

    CS_LOW();                       /* 总线上只有一个设备，片选常低 */

    TFT_Cmd(0x01);  HAL_Delay(150);                 /* SWRESET  软件复位 */
    TFT_Cmd(0x11);  HAL_Delay(150);                 /* SLPOUT   退出睡眠 */

    TFT_Cmd(0xB1);  d3[0]=0x01; d3[1]=0x2C; d3[2]=0x2D; TFT_Data(d3, 3);   /* 帧率 */
    TFT_Cmd(0xC0);  d3[0]=0xA2; d3[1]=0x02; d3[2]=0x84; TFT_Data(d3, 3);   /* 电源 */
    TFT_Cmd(0xC1);  TFT_Data1(0xC5);
    TFT_Cmd(0xC2);  d2[0]=0x0A; d2[1]=0x00; TFT_Data(d2, 2);
    TFT_Cmd(0xC3);  d2[0]=0x8A; d2[1]=0x2A; TFT_Data(d2, 2);
    TFT_Cmd(0xC4);  d2[0]=0x8A; d2[1]=0xEE; TFT_Data(d2, 2);
    TFT_Cmd(0xC5);  TFT_Data1(0x0E);

    TFT_Cmd(0x20);                                  /* INVOFF  关反显 */

#if USE_BGR
    TFT_Cmd(0x36);  TFT_Data1((uint8_t)(MADCTL_VAL | 0x08));
#else
    TFT_Cmd(0x36);  TFT_Data1((uint8_t)MADCTL_VAL);
#endif

    TFT_Cmd(0x3A);  TFT_Data1(0x05);                /* COLMOD  16bit RGB565 */

#if USE_GAMMA
    {
        uint8_t gp[16];

        gp[0]=0x02; gp[1]=0x1C; gp[2]=0x07; gp[3]=0x12;
        gp[4]=0x37; gp[5]=0x32; gp[6]=0x29; gp[7]=0x2D;
        gp[8]=0x29; gp[9]=0x25; gp[10]=0x2B; gp[11]=0x39;
        gp[12]=0x00; gp[13]=0x01; gp[14]=0x03; gp[15]=0x10;
        TFT_Cmd(0xE0);  TFT_Data(gp, 16);           /* 正极性伽马 */

        gp[0]=0x03; gp[1]=0x1D; gp[2]=0x07; gp[3]=0x06;
        gp[4]=0x2E; gp[5]=0x2C; gp[6]=0x29; gp[7]=0x2D;
        gp[8]=0x2E; gp[9]=0x2E; gp[10]=0x37; gp[11]=0x3F;
        gp[12]=0x00; gp[13]=0x00; gp[14]=0x02; gp[15]=0x10;
        TFT_Cmd(0xE1);  TFT_Data(gp, 16);           /* 负极性伽马 */
    }
#endif

    TFT_Cmd(0x13);  HAL_Delay(10);                  /* NORON   普通显示模式 */
    TFT_Cmd(0x29);  HAL_Delay(50);                  /* DISPON  开显示 */

    U_LOG("[TFT] init done\r\n");
}

/*==============================================================================
 *                         六、绘图 API
 *============================================================================*/

/* 填满整个控制器显存（0..131 / 0..161），不带偏移，用来彻底消掉边缘杂色 */
static void TFT_ClearAll(uint16_t color)
{
    uint16_t i, h;
    uint8_t d[4];

    for (i = 0; i < GRAM_W; i++)
    {
        s_line[i * 2]     = (uint8_t)(color >> 8);
        s_line[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    TFT_Cmd(0x2A);
    d[0]=0; d[1]=0; d[2]=(uint8_t)((GRAM_W - 1) >> 8); d[3]=(uint8_t)((GRAM_W - 1) & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2B);
    d[0]=0; d[1]=0; d[2]=(uint8_t)((GRAM_H - 1) >> 8); d[3]=(uint8_t)((GRAM_H - 1) & 0xFF);
    TFT_Data(d, 4);

    TFT_Cmd(0x2C);
    DC_HIGH();                       /* ★ 0x2C 之后全是像素，DC 必须是高 */

    for (h = 0; h < GRAM_H; h++)
    {
        SPI_Send(s_line, (uint16_t)(GRAM_W * 2));
    }
}

/* 填充矩形（逻辑坐标 0..127 / 0..159） */
void TFT_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint16_t i, j;

    if (w == 0 || h == 0) return;
    if (x >= LCD_W || y >= LCD_H) return;
    if ((uint32_t)x + w > LCD_W) w = (uint16_t)(LCD_W - x);
    if ((uint32_t)y + h > LCD_H) h = (uint16_t)(LCD_H - y);

    for (i = 0; i < w; i++)
    {
        s_line[i * 2]     = (uint8_t)(color >> 8);
        s_line[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    TFT_SetWindow(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    DC_HIGH();                       /* ★ 同上，漏了这一行就永远花屏 */

    for (j = 0; j < h; j++)
    {
        SPI_Send(s_line, (uint16_t)(w * 2));
    }
}

void TFT_Fill(uint16_t color)
{
    TFT_FillRect(0, 0, LCD_W, LCD_H, color);
}

void TFT_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    TFT_FillRect(x, y, 1, 1, color);
}

void TFT_DrawHLine(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    TFT_FillRect(x, y, w, 1, color);
}

void TFT_DrawVLine(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    TFT_FillRect(x, y, 1, h, color);
}

void TFT_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    TFT_FillRect(x, y, w, 1, color);
    TFT_FillRect(x, (uint16_t)(y + h - 1), w, 1, color);
    TFT_FillRect(x, y, 1, h, color);
    TFT_FillRect((uint16_t)(x + w - 1), y, 1, h, color);
}

/*==============================================================================
 *                         七、5x7 数字字体
 *   每字节一列，bit0 = 最上面那一行。只做 0~9，够显示计数器了。
 *============================================================================*/
static const uint8_t FONT_DIGIT[10][5] =
{
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },   /* 0 */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },   /* 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },   /* 2 */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 },   /* 3 */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },   /* 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },   /* 5 */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 },   /* 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },   /* 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },   /* 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1E }    /* 9 */
};

/* 画一个数字，scale 是放大倍数（1 是 5x7，3 是 15x21） */
void TFT_DrawDigit(uint16_t x, uint16_t y, uint8_t d, uint16_t fg, uint16_t bg, uint8_t scale)
{
    uint8_t col, row;
    uint8_t bits;

    if (d > 9) return;

    for (col = 0; col < 5; col++)
    {
        bits = FONT_DIGIT[d][col];
        for (row = 0; row < 7; row++)
        {
            TFT_FillRect((uint16_t)(x + col * scale),
                         (uint16_t)(y + row * scale),
                         scale, scale,
                         (bits & (1u << row)) ? fg : bg);
        }
    }
}

/* 画一个无符号数（最多 3 位），带前导 0 */
void TFT_DrawNum3(uint16_t x, uint16_t y, uint16_t v, uint16_t fg, uint16_t bg, uint8_t scale)
{
    TFT_DrawDigit((uint16_t)(x),                 y, (uint8_t)((v / 100) % 10), fg, bg, scale);
    TFT_DrawDigit((uint16_t)(x + 6 * scale),     y, (uint8_t)((v / 10) % 10),  fg, bg, scale);
    TFT_DrawDigit((uint16_t)(x + 12 * scale),    y, (uint8_t)(v % 10),         fg, bg, scale);
}

/*==============================================================================
 *                         八、演示
 *============================================================================*/

/* 1. 三原色 + 白黑，验证颜色通道
 *    注意：全屏白色是面板电流最大的时候，最容易暴露供电问题，
 *    所以白色只停 400ms，别让它长时间拉满。 */
static void Demo_Colors(void)
{
    U_LOG("  RED\r\n");     TFT_Fill(C_RED);      HAL_Delay(700);
    U_LOG("  GREEN\r\n");   TFT_Fill(C_GREEN);    HAL_Delay(700);
    U_LOG("  BLUE\r\n");    TFT_Fill(C_BLUE);     HAL_Delay(700);
    U_LOG("  WHITE\r\n");   TFT_Fill(C_WHITE);    HAL_Delay(400);
    U_LOG("  BLACK\r\n");   TFT_Fill(C_BLACK);    HAL_Delay(300);
}

/* 2. 八色彩条，验证列方向 */
static void Demo_Bars(void)
{
    uint16_t colors[8];
    uint8_t i;
    uint16_t bw;


    colors[0] = C_WHITE;
    colors[1] = C_YELLOW;
    colors[2] = C_CYAN;
    colors[3] = C_GREEN;
    colors[4] = C_MAGENTA;
    colors[5] = C_RED;
    colors[6] = C_BLUE;
    colors[7] = C_BLACK;

    bw = (uint16_t)(LCD_W / 8);        /* 16 */

    for (i = 0; i < 8; i++)
    {
        TFT_FillRect((uint16_t)(i * bw), 0, bw, LCD_H, colors[i]);
    }
    HAL_Delay(2500);
}

/* 3. 边框 + 四角标记，验证偏移和方向
 *    正常：四个角各有一个 12x12 方块，边框完整贴边
 *    偏移：某一侧边框被切掉 / 外侧多出一条杂色                              */
static void Demo_Border(void)
{

    TFT_Fill(C_BLACK);
    TFT_DrawRect(0, 0, LCD_W, LCD_H, C_WHITE);
    TFT_DrawRect(1, 1, (uint16_t)(LCD_W - 2), (uint16_t)(LCD_H - 2), C_GRAY);

    TFT_FillRect(2, 2, 12, 12, C_RED);                        /* 左上 红 */
    TFT_FillRect((uint16_t)(LCD_W - 14), 2, 12, 12, C_GREEN); /* 右上 绿 */
    TFT_FillRect(2, (uint16_t)(LCD_H - 14), 12, 12, C_BLUE);  /* 左下 蓝 */
    TFT_FillRect((uint16_t)(LCD_W - 14), (uint16_t)(LCD_H - 14), 12, 12, C_YELLOW); /* 右下 黄 */

    /* 中十字，验证中心对齐 */
    TFT_DrawHLine(0, (uint16_t)(LCD_H / 2), LCD_W, C_GRAY);
    TFT_DrawVLine((uint16_t)(LCD_W / 2), 0, LCD_H, C_GRAY);

    HAL_Delay(3000);
}

/* 4. 横向渐变，验证 16bit 色深 */
static void Demo_Gradient(void)
{
    uint16_t x;
    uint8_t r, g, b;


    for (x = 0; x < LCD_W; x++)
    {
        r = (uint8_t)(255 - (x * 255 / (LCD_W - 1)));
        g = (uint8_t)(x * 255 / (LCD_W - 1));
        b = (uint8_t)((x < 64) ? (x * 4) : ((LCD_W - 1 - x) * 4));

        TFT_FillRect(x, 0, 1, LCD_H, RGB565(r, g, b));
    }
    HAL_Delay(2500);
}

/* 5. 棋盘格，验证像素对齐（格子必须方正、边界清晰） */
static void Demo_Checker(void)
{
    uint16_t x, y;
    uint8_t on;


    for (y = 0; y < LCD_H; y += 8)
    {
        for (x = 0; x < LCD_W; x += 8)
        {
            on = (uint8_t)(((x / 8) + (y / 8)) & 1);
            TFT_FillRect(x, y, 8, 8, on ? C_WHITE : C_BLACK);
        }
    }
    HAL_Delay(2500);
}

/* 6. 计数器，验证局部刷新（数字应该每秒 +1，不闪、不残留）
 *    同时兼当心跳：串口每秒打一个点，说明 MCU 还活着。
 *    如果屏幕白了但串口还在打点 -> 面板被复位了，不是程序死了。 */
static void Demo_Counter(void)
{
    uint16_t n;

    TFT_Fill(C_BLACK);
    TFT_DrawRect(0, 0, LCD_W, LCD_H, C_BLUE);

    for (n = 0; n < 1000; n++)
    {
        TFT_DrawNum3(19, 60, (uint16_t)(n % 1000), C_WHITE, C_BLACK, 6);
        U_LOG(".");                  /* 心跳 */
        if ((n % 40) == 39)
        {
            U_LOG("\r\n");
        }
        HAL_Delay(1000);
    }
}

/* 7. 移动方块，验证连续刷新不撕裂 */
static void Demo_MovingBox(void)
{
    int16_t x = 0;
    int16_t y = 0;
    int16_t dx = 2;
    int16_t dy = 2;
    uint16_t n;


    TFT_Fill(C_BLACK);

    for (n = 0; n < 400; n++)
    {
        TFT_FillRect((uint16_t)x, (uint16_t)y, 20, 20, C_BLACK);
        x = (int16_t)(x + dx);
        y = (int16_t)(y + dy);
        if (x <= 0 || x >= (LCD_W - 20)) { dx = (int16_t)(-dx); if (x < 0) x = 0; if (x > LCD_W - 20) x = LCD_W - 20; }
        if (y <= 0 || y >= (LCD_H - 20)) { dy = (int16_t)(-dy); if (y < 0) y = 0; if (y > LCD_H - 20) y = LCD_H - 20; }
        TFT_FillRect((uint16_t)x, (uint16_t)y, 20, 20, C_ORANGE);
        HAL_Delay(12);
    }
}

/*==============================================================================
 *                         九、静态保持诊断（RUN_DIAG = 1）
 *
 *   目的：把「闪几个颜色就白」这个模糊现象，变成三个可以回答的问题。
 *
 *   问题 1：面板在「不写数据」的时候，会不会自己变白？
 *           填一次红色，然后 30 秒什么都不做，每秒打一个点。
 *           · 30 秒后还是红的  -> 面板不会自己复位，复位是被"写操作"触发的
 *           · 几秒后变白        -> 面板会自己复位（RES 悬空 / 供电不稳）
 *
 *   问题 2：变白之后，面板还认不认之前的初始化？
 *           不重新初始化，直接再填一次绿色。
 *           · 变绿 -> 面板还在初始化状态，我们的写操作是好的
 *           · 还是白 -> 面板丢了初始化（被复位过）
 *
 *   问题 3：重新初始化能不能救回来？
 *           重新 init 后填蓝色。
 *           · 变蓝 -> 能救 -> 说明就是"被复位"，只要每步重新 init 就能一直跑
 *
 *   跑完这三个问题，再进入慢速彩色循环（每色 2 秒，每次都重新 init），
 *   这时候如果一直正常，就说明"每步重新 init"这个策略是有效的。
 *============================================================================*/
static void Diag_Static(void)
{
    uint16_t i;

    U_LOG("\r\n########## DIAG START ##########\r\n");

    /* ---------------- 问题 1 ---------------- */
    TFT_Init();
    TFT_ClearAll(C_BLACK);
    TFT_Fill(C_RED);

    U_LOG("[Q1] RED shown, then 30 s of NOTHING.\r\n");
    U_LOG("     watch: does it turn WHITE by itself ?\r\n");
    for (i = 0; i < 30; i++)
    {
        U_LOG(".");
        HAL_Delay(1000);
    }
    U_LOG("\r\n[Q1] 30 s done.\r\n");
    U_LOG("     still RED  -> panel does NOT self-reset\r\n");
    U_LOG("     WHITE      -> panel self-reset (RES / power)\r\n");

    /* ---------------- 问题 2 ---------------- */
    TFT_Fill(C_GREEN);            /* 故意不重新初始化 */
    U_LOG("[Q2] GREEN sent WITHOUT re-init. 10 s ...\r\n");
    for (i = 0; i < 10; i++)
    {
        U_LOG(".");
        HAL_Delay(1000);
    }
    U_LOG("\r\n[Q2] turned GREEN -> panel still initialised, our writes are fine\r\n");
    U_LOG("     stayed WHITE -> panel lost its init (it was reset)\r\n");

    /* ---------------- 问题 3 ---------------- */
    TFT_Init();
    TFT_Fill(C_BLUE);
    U_LOG("[Q3] BLUE after RE-INIT. 10 s ...\r\n");
    for (i = 0; i < 10; i++)
    {
        U_LOG(".");
        HAL_Delay(1000);
    }
    U_LOG("\r\n[Q3] turned BLUE -> re-init recovers it\r\n");
    U_LOG("     still WHITE -> re-init does NOT recover it\r\n");

    /* ---------------- 慢速循环 ---------------- */
    U_LOG("\r\n[LOOP] slow colours, re-init every time (2 s each)\r\n");

    while (1)
    {
        TFT_Init();  TFT_Fill(C_RED);    U_LOG("R");  HAL_Delay(2000);
        TFT_Init();  TFT_Fill(C_GREEN);  U_LOG("G");  HAL_Delay(2000);
        TFT_Init();  TFT_Fill(C_BLUE);   U_LOG("B");  HAL_Delay(2000);
        TFT_Init();  TFT_Fill(C_WHITE);  U_LOG("W");  HAL_Delay(2000);
        TFT_Init();  TFT_Fill(C_BLACK);  U_LOG("K");  HAL_Delay(2000);
    }
}

/*==============================================================================
 *                         十、Error_Handler（只为满足工程里的引用）
 *============================================================================*/
void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}

/*==============================================================================
 *                         十、main
 *============================================================================*/
int main(void)
{
    HAL_Init();
    SystemClock_Config();

#if USE_SERIAL
    USART1_InitRaw(84000000u, 115200u);
    U_LOG("\r\n\r\n=== ST7735 128x160 demo ===\r\n");
    U_LOG("SCK=PB10  SDA=PC3  CS=PC15  DC=PC14\r\n");
    U_LOG("SPI_BR=");
    U_LOG_N(SPI_BR);
    U_LOG("  MADCTL=");
    U_LOG_N(MADCTL_VAL);
    U_LOG("\r\n");
#endif

    TFT_Init();
    TFT_ClearAll(C_BLACK);            /* 先清整个显存，消掉边缘杂色 */

#if RUN_DIAG
    Diag_Static();                    /* 诊断模式，永不返回 */
#else
    /* ★ 每一段演示前都重新初始化一次。
     *   成本只有 0.4 秒，但换来一个巨大的好处：
     *   万一面板被噪声复位了（RES 悬空 / 电源波动），下一段会自动救回来，
     *   不会出现"卡在一片白屏再也回不来"的情况。
     *   测试程序 MODE 8 就是这么干的，所以它从来没卡住过。 */
    while (1)
    {
        U_LOG("[step] colours\r\n");
        TFT_Init();
        Demo_Colors();

        U_LOG("[step] bars\r\n");
        TFT_Init();
        Demo_Bars();

        U_LOG("[step] border\r\n");
        TFT_Init();
        Demo_Border();

        U_LOG("[step] gradient\r\n");
        TFT_Init();
        Demo_Gradient();

        U_LOG("[step] checker\r\n");
        TFT_Init();
        Demo_Checker();

        U_LOG("[step] moving box\r\n");
        TFT_Init();
        Demo_MovingBox();

        U_LOG("[step] counter\r\n");
        TFT_Init();
        Demo_Counter();               /* 计数器放在最后，会一直跑 */
    }
#endif
}
