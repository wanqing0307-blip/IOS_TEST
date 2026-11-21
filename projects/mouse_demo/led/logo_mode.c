/*******************************************************************************
* Copyright (c) 2013, SHENZHEN SEMITEK Co.,LTD.
* All rights reserved.
*
* 文件名称: led.c
*
* 功能说明:
* 修改说明:
*******************************************************************************/
#include <string.h>
#include "om_driver.h"

#include "main.h"
#include "led.h"
#include "led_logo_tbl.h"

//-----------------------------------------------------------------------------;
// 常量定义区
//

//-----------------------------------------------------------------------------;
// 变量定义区
//


bool                            bled_run_dir;

uint16_t                        lfsr_value;
uint8_t                         snake_led_index;

//-----------------------------------------------------------------------------;
// 表格定义区
//

#define iHUE_NUM        48

// 48 阶平滑色环（R,G,B，满量程 iPWM_MAX）
static const unsigned char hue_rgb_tbl[iHUE_NUM][3] = {
    {64,  0,  0}, {64,  8,  0}, {64, 16,  0}, {64, 24,  0}, {64, 32,  0}, {64, 40,  0},
    {64, 48,  0}, {64, 56,  0}, {64, 64,  0}, {56, 64,  0}, {48, 64,  0}, {40, 64,  0},
    {32, 64,  0}, {24, 64,  0}, {16, 64,  0}, { 8, 64,  0}, { 0, 64,  0}, { 0, 64,  8},
    { 0, 64, 16}, { 0, 64, 24}, { 0, 64, 32}, { 0, 64, 40}, { 0, 64, 48}, { 0, 64, 56},
    { 0, 64, 64}, { 0, 56, 64}, { 0, 48, 64}, { 0, 40, 64}, { 0, 32, 64}, { 0, 24, 64},
    { 0, 16, 64}, { 0,  8, 64}, { 0,  0, 64}, { 8,  0, 64}, {16,  0, 64}, {24,  0, 64},
    {32,  0, 64}, {40,  0, 64}, {48,  0, 64}, {56,  0, 64}, {64,  0, 64}, {64,  0, 56},
    {64,  0, 48}, {64,  0, 40}, {64,  0, 32}, {64,  0, 24}, {64,  0, 16}, {64,  0,  8},
};

//-----------------------------------------------------------------------------;
// 灯效时间基准：所有灯效按 RTC 实际时间推进，不再按 logo_process 调用次数计数
// （各模式调用频率不一致、写 flash 时会跳过调用，按次数计数会忽快忽慢、起步变慢）
//
#define iLED_BASE_TICK          (mMS_TO_TICK(15) >> 1)      // 基准节拍 7.5ms（原 BLE/USB 下的调用周期）
#define iLED_CATCHUP_MAX        8                           // 连续灯效单次最多补走的步数
#define iLED_SPEED_LEV_CLAMP    4                           // 速度档上限保护（flash 读出可达 7）

typedef struct {
    uint32_t    last;                                       // 上一步的 RTC 时刻
    bool        refresh;                                    // 置位后下一次调用立即走一步
} led_timer_t;

static led_timer_t              led_timer_main;             // 灯效主节拍

/******************************************************************************
* 函数名称: led_timer_reset
* 功能描述: 灯效时间基准复位（切换灯效时调用），首步立即执行
******************************************************************************/
void led_timer_reset(void)
{
    led_timer_main.refresh = true;
}

/******************************************************************************
* 函数名称: led_timer_take
* 功能描述: 取出到期的步数
* 输入参数: t        - 定时器
*           period   - 每步周期（RTC tick）
*           max_step - 单次最多返回的步数；1 表示离散跳帧类灯效，不补帧
* 输出参数: 本次应执行的步数（0 表示未到时间）
* 说    明: 落后超过 max_step 步（写 flash 阻塞、关灯/休眠/回连暂停后恢复）时
*           以当前时刻重新对齐，避免恢复瞬间快进
******************************************************************************/
static uint8_t led_timer_take(led_timer_t *t, uint32_t period, uint8_t max_step)
{
    uint32_t now = drv_rtc_read(OM_RTC);
    uint32_t n;

    if(t->refresh)
    {
        t->refresh = false;
        t->last = now;
        return 1;
    }

    n = (now - t->last) / period;
    if(0 == n)
        return 0;

    if(max_step < n)
    {
        t->last = now;
        return max_step;
    }

    t->last += n * period;
    return (uint8_t)n;
}

// 当前速度档（带越界保护），0 最快
static uint8_t led_speed_index(void)
{
    return (iLED_SPEED_LEV_CLAMP < led_speed_lev) ? iLED_SPEED_LEV_CLAMP : led_speed_lev;
}

//=============================================================================;
// 代码区                                                                      ;
//=============================================================================;

/******************************************************************************
* 函数名称: led_hint_process
* 功能描述:
* 输入参数:
* 输出参数:
* 说    明:
******************************************************************************/
void led_mode_on(void)
{
    unsigned char h;

    rgb_enable = rgb_en_tbl[led_rgb_index&0x07];
    for(gtemp_j=0; gtemp_j<iLED_MAX; gtemp_j++)
    {
        if(iRGB_QICAI == (led_rgb_index&0x07))
        {
            // 平滑色环：8 颗灯色相依次相差 45°
            h = (gtemp_j*(iHUE_NUM/iLED_MAX)) % iHUE_NUM;

            pwm_rgb_tbl[gtemp_j][iR_INDEX] = hue_rgb_tbl[h][0];
            pwm_rgb_tbl[gtemp_j][iG_INDEX] = hue_rgb_tbl[h][1];
            pwm_rgb_tbl[gtemp_j][iB_INDEX] = hue_rgb_tbl[h][2];
            continue;
        }
        rgb_w_max();
    }
}

//-----------------------------------------------------------------------------;
// 呼吸
//
// 呼吸周期(基准节拍 7.5ms 数)，下标 = 速度档：约 3s / 4.5s / 6s / 8s / 10s
static const unsigned short breath_cycle_tbl[] = { 400, 600, 800, 1067, 1333 };

// 一个周期按 0~1023 划分：渐亮 42.5% -> 最亮停顿 5% -> 渐暗 42.5% -> 全暗停顿 10%
#define iBREATH_POS_MAX         1024
#define iBREATH_RISE_END        435
#define iBREATH_TOP_END         486
#define iBREATH_FALL_END        921
#define iBREATH_RAMP_LEN        (iBREATH_RISE_END)          // 渐亮/渐暗段长度

// 呼吸亮度曲线（33 点，0~4096）：sin(πx/2)^2.2
// 感知亮度正弦缓出：离开全暗快、接近最亮时变化率趋于 0，顶部不生硬折返；
// 再经 γ2.2 转为灯珠线性亮度
#define iBREATH_LUM_MAX         4096
static const unsigned short breath_curve_tbl[33] = {
       0,    5,   25,   60,  112,  182,  270,  374,  495,  632,  783,
     948, 1124, 1310, 1505, 1706, 1911, 2118, 2325, 2529, 2729, 2922,
    3107, 3280, 3441, 3588, 3718, 3831, 3925, 3999, 4053, 4085, 4096,
};

static unsigned short           breath_phase;               // 周期内相位（基准节拍数）
static unsigned char            breath_hue;                 // 七彩时色环起点，每个周期在全暗处推进

/******************************************************************************
* 函数名称: led_mode_breath_init
* 功能描述: 呼吸进入初始化（切换灯效时调用），从全暗开始渐亮
******************************************************************************/
void led_mode_breath_init(void)
{
    breath_phase = 0;
    breath_hue = 0;
}

// x: 0~1023 -> 亮度 0~4096（查表 + 线性插值）
static unsigned short breath_curve(unsigned short x)
{
    unsigned char  idx  = x >> 5;
    unsigned char  frac = x & 0x1F;
    unsigned short a    = breath_curve_tbl[idx];

    return a + (((breath_curve_tbl[idx+1] - a) * frac) >> 5);
}

/******************************************************************************
* 函数名称: led_mode_breath
* 功能描述: 呼吸：平滑渐亮 -> 短暂停顿 -> 平滑渐暗 -> 全暗停顿
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进相位；亮度先以 12bit 精度计算、与颜色相乘后
*           一次性四舍五入到 0~64，避免两次截断造成的暗端台阶与色偏。
*           led_rgb_index 为七彩时 8 颗灯色环呼吸（每周期旋转 22.5°），
*           否则按所选单色呼吸（与常亮一致）
******************************************************************************/
void led_mode_breath(void)
{
    unsigned char  i, n, h, en;
    unsigned short cycle, pos, x;
    uint32_t       lum;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    cycle = breath_cycle_tbl[led_speed_index()];
    breath_phase += n;
    if(cycle <= breath_phase)
    {
        breath_phase %= cycle;                  // 也兼容运行中切换到更短周期
        breath_hue = (breath_hue + 3) % iHUE_NUM;   // 全暗时换色，看不到跳变
    }

    pos = ((uint32_t)breath_phase * iBREATH_POS_MAX) / cycle;

    if(pos < iBREATH_RISE_END)
    {
        x   = ((uint32_t)pos * 1024) / iBREATH_RAMP_LEN;
        lum = breath_curve(x);
    }
    else if(pos < iBREATH_TOP_END)
    {
        lum = iBREATH_LUM_MAX;
    }
    else if(pos < iBREATH_FALL_END)
    {
        x   = ((uint32_t)(iBREATH_FALL_END - 1 - pos) * 1024) / iBREATH_RAMP_LEN;
        lum = breath_curve(x);
    }
    else
    {
        lum = 0;
    }

    en = rgb_en_tbl[led_rgb_index&0x07];
    for(i=0; i<iLED_MAX; i++)
    {
        unsigned char r, g, b;

        if(iRGB_QICAI == (led_rgb_index&0x07))
        {
            // 色环：8 颗灯色相依次相差 45°
            h = (breath_hue + i*(iHUE_NUM/iLED_MAX)) % iHUE_NUM;
            r = hue_rgb_tbl[h][0];
            g = hue_rgb_tbl[h][1];
            b = hue_rgb_tbl[h][2];
        }
        else
        {
            r = (en & iPWM_R) ? iPWM_MAX : 0;
            g = (en & iPWM_G) ? iPWM_MAX : 0;
            b = (en & iPWM_B) ? iPWM_MAX : 0;
        }

        pwm_rgb_tbl[i][iR_INDEX] = (r * lum + (iBREATH_LUM_MAX/2)) / iBREATH_LUM_MAX;
        pwm_rgb_tbl[i][iG_INDEX] = (g * lum + (iBREATH_LUM_MAX/2)) / iBREATH_LUM_MAX;
        pwm_rgb_tbl[i][iB_INDEX] = (b * lum + (iBREATH_LUM_MAX/2)) / iBREATH_LUM_MAX;
    }
}

//-----------------------------------------------------------------------------;
// 霓虹渐变
//
// 整条灯带同色，沿色环 红->黄->绿->青->蓝->紫 平滑循环
// 色环转一圈的时间(基准节拍 7.5ms 数)，下标 = 速度档：约 2.88s / 5.76s / 8.64s，与原速度一致
static const unsigned short nhjb_cycle_tbl[] = { 384, 768, 1152, 1536, 1920 };

static uint16_t                 nhjb_phase;                 // 周期内相位（基准节拍数）

/******************************************************************************
* 函数名称: led_mode_nhjb_init
* 功能描述: 霓虹渐变进入初始化（切换灯效时调用），从红色开始
******************************************************************************/
void led_mode_nhjb_init(void)
{
    nhjb_phase = 0;
}

/******************************************************************************
* 函数名称: led_mode_nhjb
* 功能描述: 霓虹渐变：整条灯带沿色环平滑变色
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进；48 阶色环插值。
*           黄/青/紫等双通道满量程混色的功率是纯色的 2 倍，原效果每到这些颜色
*           整条灯带明显变亮、像在闪；这里按通道和做等功率压缩（双通道全亮时
*           压到 0.707 倍），整体亮度更平稳
******************************************************************************/
void led_mode_nhjb(void)
{
    uint8_t  i, n, h0, h1, hf;
    uint16_t cycle, hue, sum, r, g, b;
    uint32_t k;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    cycle = nhjb_cycle_tbl[led_speed_index()];
    nhjb_phase += n;
    while(cycle <= nhjb_phase)
        nhjb_phase -= cycle;

    hue = ((uint32_t)nhjb_phase * (iHUE_NUM * 256)) / cycle;
    h0  = hue >> 8;
    hf  = hue & 0xFF;
    h1  = (iHUE_NUM-1 <= h0) ? 0 : (h0 + 1);

    // 颜色 ×256 精度
    r = hue_rgb_tbl[h0][0] * (256 - hf) + hue_rgb_tbl[h1][0] * hf;
    g = hue_rgb_tbl[h0][1] * (256 - hf) + hue_rgb_tbl[h1][1] * hf;
    b = hue_rgb_tbl[h0][2] * (256 - hf) + hue_rgb_tbl[h1][2] * hf;

    // 等功率压缩：k = 64 / (64 + (sum-64)·0.414)，sum 为通道和（64~128）
    sum = (r + g + b) >> 8;
    if(sum < iPWM_MAX)
        sum = iPWM_MAX;                             // 插值取整可能差 1，防止下面减成负数
    k   =(64UL << 12) / (64 + (((uint32_t)(sum - iPWM_MAX) * 106) >> 8));   // 0.414 ≈ 106/256

    for(i=0; i<iLED_MAX; i++)
    {
        pwm_rgb_tbl[i][iR_INDEX] = ((uint32_t)r * k + (1UL << 19)) >> 20;
        pwm_rgb_tbl[i][iG_INDEX] = ((uint32_t)g * k + (1UL << 19)) >> 20;
        pwm_rgb_tbl[i][iB_INDEX] = ((uint32_t)b * k + (1UL << 19)) >> 20;
    }
}

//=============================================================================;
// 代码区                                                                      ;
//=============================================================================;

//-----------------------------------------------------------------------------;
// 流光
//
// 亮带波长 = 4 颗灯（8 颗灯上同时有两条亮带），向高序号方向流动
// 流过一个波长的时间(基准节拍 7.5ms 数)，下标 = 速度档：约 0.96s / 1.92s / 2.88s，与原速度一致
static const unsigned short lg_period_tbl[] = { 128, 256, 384, 512, 640 };

#define iLG_WAVE_LED        4                               // 波长（灯数）
#define iLG_POS_MAX         1024                            // 一个波长的相位细分
#define iLG_HUE_SPAN        (iHUE_NUM * 256)                // 色环相位（8.8 定点）
#define iLG_HUE_PER_WAVE    (iLG_HUE_SPAN / 8)              // 每流过一个波长色环转 1/8 圈

// 亮带截面（半个波长，33 点，0~4096）：((1+cos)/2)^2.2
// 亮带中心最亮、两侧快速变暗，带间留出暗隙，流动感清晰
static const unsigned short lg_band_tbl[33] = {
    4096, 4074, 4010, 3904, 3761, 3583, 3375, 3142, 2891, 2627, 2356,
    2085, 1818, 1562, 1319, 1095,  891,  710,  553,  419,  308,  219,
     150,   97,   60,   34,   18,    8,    3,    1,    0,    0,    0,
};

static unsigned short           lg_phase;                   // 波相位（基准节拍数，0 ~ 周期-1）
static unsigned short           lg_hue;                     // 七彩色环相位（8.8 定点）

/******************************************************************************
* 函数名称: led_mode_liuguang_init
* 功能描述: 流光进入初始化（切换灯效时调用）
******************************************************************************/
void led_mode_liuguang_init(void)
{
    lg_phase = 0;
    lg_hue = 0;
}

// x: 距亮带中心的相位 0~1023（循环）-> 亮度 0~4096
static unsigned short lg_band(unsigned short x)
{
    unsigned char  idx, frac;
    unsigned short a;

    if(iLG_POS_MAX/2 < x)
        x = iLG_POS_MAX - x;                        // 对称：只用半个波长的表
    x <<= 1;                                        // 0~1024
    if(1024 <= x)
        return 0;

    idx  = x >> 5;
    frac = x & 0x1F;
    a    = lg_band_tbl[idx];
    return a - (((a - lg_band_tbl[idx+1]) * frac) >> 5);
}

/******************************************************************************
* 函数名称: led_mode_liuguang
* 功能描述: 流光：两条柔和亮带沿灯带连续流动
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进相位；七彩时颜色沿灯带渐变（相邻灯 45°）并随流动
*           缓慢旋转，否则按 led_rgb_index 所选单色流动（与常亮一致）
******************************************************************************/
void led_mode_liuguang(void)
{
    unsigned char  i, n, h0, h1, hf, en;
    unsigned short period, pos, x, hue;
    uint32_t       lum;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    period = lg_period_tbl[led_speed_index()];

    // 色环随波相位同步推进：每流过一个波长转 1/8 圈
    lg_hue = (lg_hue + ((uint32_t)iLG_HUE_PER_WAVE * n) / period) % iLG_HUE_SPAN;

    lg_phase += n;
    while(period <= lg_phase)
        lg_phase -= period;                         // 也兼容运行中切换到更短周期

    pos = ((uint32_t)lg_phase * iLG_POS_MAX) / period;
    en  = rgb_en_tbl[led_rgb_index&0x07];

    for(i=0; i<iLED_MAX; i++)
    {
        unsigned short r, g, b;

        // 灯 i 比灯 i-1 晚 1/4 波长到达亮带中心 -> 亮带向高序号流动
        x   = (pos + iLG_POS_MAX - (i % iLG_WAVE_LED) * (iLG_POS_MAX / iLG_WAVE_LED)) % iLG_POS_MAX;
        lum = lg_band(x);

        if(iRGB_QICAI == (led_rgb_index&0x07))
        {
            // 色相插值：相邻灯相差 45°，整体随时间旋转
            hue = (lg_hue + (uint32_t)i * (iLG_HUE_SPAN / iLED_MAX)) % iLG_HUE_SPAN;
            h0  = hue >> 8;
            hf  = hue & 0xFF;
            h1  = (iHUE_NUM-1 <= h0) ? 0 : (h0 + 1);
            r   = (hue_rgb_tbl[h0][0] * (256 - hf) + hue_rgb_tbl[h1][0] * hf) >> 8;
            g   = (hue_rgb_tbl[h0][1] * (256 - hf) + hue_rgb_tbl[h1][1] * hf) >> 8;
            b   = (hue_rgb_tbl[h0][2] * (256 - hf) + hue_rgb_tbl[h1][2] * hf) >> 8;
        }
        else
        {
            r = (en & iPWM_R) ? iPWM_MAX : 0;
            g = (en & iPWM_G) ? iPWM_MAX : 0;
            b = (en & iPWM_B) ? iPWM_MAX : 0;
        }

        pwm_rgb_tbl[i][iR_INDEX] = (r * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iG_INDEX] = (g * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iB_INDEX] = (b * lum + 2048) >> 12;
    }
}

//-----------------------------------------------------------------------------;
// 繁星点点
//
// 每颗星：快速亮起（寿命前 20%）-> 柔和熄灭（后 80%），γ2.2 校正，像真正的“一闪”
// 新星只落在空闲的灯上，出现间隔带 ±50% 随机抖动
//
// 新星平均出现间隔(基准节拍 7.5ms 数)，下标 = 速度档：约 150 / 300 / 525ms（与原设定一致）
static const unsigned char star_spawn_tbl[] = { 20, 40, 70, 110, 160 };
// 单颗星寿命(基准节拍数)，下标 = 速度档：约 0.6 / 0.72 / 0.96s
static const unsigned char star_life_tbl[]  = { 80, 96, 128, 160, 192 };

#define iSTAR_IDLE          0xFF                            // 该灯无星

// 星亮度随寿命变化（33 点，0~4096）
static const unsigned short star_curve_tbl[33] = {
       0,  182,  783, 1706, 2729, 3588, 4053, 4090, 4053, 3982, 3880,
    3748, 3588, 3402, 3195, 2969, 2729, 2478, 2221, 1962, 1706, 1456,
    1216,  991,  783,  596,  432,  294,  182,   98,   40,    9,    0,
};

static uint8_t                  star_age[iLED_MAX];         // 星龄(基准节拍数)，iSTAR_IDLE 表示空闲
static uint8_t                  star_hue[iLED_MAX];         // 七彩时每颗星的色相（0~47）
static int16_t                  star_next;                  // 距下一颗新星的节拍数

// 16 位 Galois LFSR（多项式 0xB400，最长周期 65535）
static uint16_t star_rand(void)
{
    lfsr_value = (lfsr_value >> 1) ^ ((uint16_t)(-(int16_t)(lfsr_value & 0x01)) & 0xB400);
    return lfsr_value;
}

/******************************************************************************
* 函数名称: led_mode_star_init
* 功能描述: 繁星进入初始化（切换灯效时调用），立即出第一颗星
******************************************************************************/
void led_mode_star_init(void)
{
    uint8_t i;

    for(i=0; i<iLED_MAX; i++)
        star_age[i] = iSTAR_IDLE;
    if(0 == lfsr_value)
        lfsr_value = 0x55AA;                        // LFSR 不能为 0
    star_next = 0;
}

// 在空闲灯里随机点亮一颗新星；全部亮着则本次不出
static void led_mode_star_spawn(void)
{
    uint8_t i, cnt = 0, pick;
    uint8_t idle[iLED_MAX];

    for(i=0; i<iLED_MAX; i++)
    {
        if(iSTAR_IDLE == star_age[i])
            idle[cnt++] = i;
    }
    if(0 == cnt)
        return;

    pick = idle[star_rand() % cnt];
    star_age[pick] = 0;
    star_hue[pick] = star_rand() % iHUE_NUM;
}

/******************************************************************************
* 函数名称: led_mode_star
* 功能描述: 繁星点点：随机位置的星点快速亮起、柔和熄灭
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进；七彩时每颗星随机取色环颜色，
*           否则按 led_rgb_index 所选单色（与常亮一致）
******************************************************************************/
void led_mode_star(void)
{
    uint8_t  i, n, lev, life, en, idx, frac;
    uint16_t x, a;
    uint32_t lum;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    lev  = led_speed_index();
    life = star_life_tbl[lev];

    // 星龄推进，寿命到则熄灭
    for(i=0; i<iLED_MAX; i++)
    {
        if(iSTAR_IDLE == star_age[i])
            continue;
        if(life <= (uint16_t)star_age[i] + n)
            star_age[i] = iSTAR_IDLE;
        else
            star_age[i] += n;
    }

    // 新星：间隔 = 平均间隔 × (0.5 ~ 1.5) 随机
    star_next -= n;
    if(star_next <= 0)
    {
        led_mode_star_spawn();
        star_next = (star_spawn_tbl[lev] >> 1) + (star_rand() % (star_spawn_tbl[lev] + 1));
    }

    en = rgb_en_tbl[led_rgb_index&0x07];
    for(i=0; i<iLED_MAX; i++)
    {
        uint16_t r, g, b;

        if(iSTAR_IDLE == star_age[i])
        {
            pwm_rgb_tbl[i][iR_INDEX] = 0;
            pwm_rgb_tbl[i][iG_INDEX] = 0;
            pwm_rgb_tbl[i][iB_INDEX] = 0;
            continue;
        }

        x    = ((uint16_t)star_age[i] * 1024) / life;   // 0~1023
        idx  = x >> 5;
        frac = x & 0x1F;
        a    = star_curve_tbl[idx];
        lum  = (int32_t)a + ((((int32_t)star_curve_tbl[idx+1] - a) * frac) >> 5);

        if(iRGB_QICAI == (led_rgb_index&0x07))
        {
            r = hue_rgb_tbl[star_hue[i]][0];
            g = hue_rgb_tbl[star_hue[i]][1];
            b = hue_rgb_tbl[star_hue[i]][2];
        }
        else
        {
            r = (en & iPWM_R) ? iPWM_MAX : 0;
            g = (en & iPWM_G) ? iPWM_MAX : 0;
            b = (en & iPWM_B) ? iPWM_MAX : 0;
        }

        pwm_rgb_tbl[i][iR_INDEX] = (r * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iG_INDEX] = (g * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iB_INDEX] = (b * lum + 2048) >> 12;
    }
}

//-----------------------------------------------------------------------------;
// 随波逐流
//
// 颜色：完整色环沿灯带流动，相邻灯相差 37.5°（8 颗灯覆盖约 300°），与原效果一致
// 波浪：一条柔和的亮度涌浪（波长 = 8 颗灯）比颜色稍快地推过灯带，颜色随波而动
//
// 色环转一圈的时间(基准节拍 7.5ms 数)，下标 = 速度档：约 1.44s / 2.88s / 4.32s，与原速度一致
static const unsigned short sbzl_cycle_tbl[] = { 192, 384, 576, 768, 960 };

#define iSBZL_HUE_SPAN      (iHUE_NUM * 256)                // 色环相位（8.8 定点）
#define iSBZL_HUE_OFFSET    (iSBZL_HUE_SPAN * 375 / 3600)   // 相邻灯 37.5°
#define iSBZL_POS_MAX       1024                            // 涌浪一个波长的相位细分
#define iSBZL_WAVE_NUM      3                               // 涌浪周期 = 色环周期 × 3/4（比颜色快）
#define iSBZL_WAVE_DEN      4

// 涌浪截面（半个波长，33 点，0~4096）：(0.55 + 0.45·(1+cos)/2)^2.2
// 浪峰全亮，浪谷约 27%（不熄灭），起伏柔和
static const unsigned short sbzl_wave_tbl[33] = {
    4096, 4086, 4057, 4009, 3943, 3860, 3762, 3650, 3526, 3391, 3249,
    3100, 2948, 2793, 2639, 2487, 2338, 2194, 2056, 1926, 1804, 1691,
    1588, 1494, 1410, 1337, 1273, 1220, 1176, 1142, 1119, 1104, 1099,
};

static unsigned short           sbzl_hue_phase;             // 色环相位（基准节拍数，0 ~ 色环周期-1）
static unsigned short           sbzl_wave_phase;            // 涌浪相位（基准节拍数，0 ~ 涌浪周期-1）

/******************************************************************************
* 函数名称: led_mode_sbzl_init
* 功能描述: 随波逐流进入初始化（切换灯效时调用）
******************************************************************************/
void led_mode_sbzl_init(void)
{
    sbzl_hue_phase = 0;
    sbzl_wave_phase = 0;
}

// x: 距浪峰的相位 0~1023（循环）-> 亮度 1099~4096
static unsigned short sbzl_wave(unsigned short x)
{
    unsigned char  idx, frac;
    unsigned short a;

    if(iSBZL_POS_MAX/2 < x)
        x = iSBZL_POS_MAX - x;                      // 对称：只用半个波长的表
    x <<= 1;                                        // 0~1024
    if(1024 <= x)
        return sbzl_wave_tbl[32];

    idx  = x >> 5;
    frac = x & 0x1F;
    a    = sbzl_wave_tbl[idx];
    return a - (((a - sbzl_wave_tbl[idx+1]) * frac) >> 5);
}

/******************************************************************************
* 函数名称: led_mode_sbzl
* 功能描述: 随波逐流：色环沿灯带流动，柔和涌浪推着颜色前进
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进相位，颜色与涌浪均向高序号流动；
*           七彩时为色环，否则按 led_rgb_index 所选单色随波起伏（与常亮一致）
******************************************************************************/
void led_mode_sbzl(void)
{
    unsigned char  i, n, h0, h1, hf, en;
    unsigned short cycle, wave_cycle, hue, wpos, x;
    uint32_t       lum;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    cycle      = sbzl_cycle_tbl[led_speed_index()];
    wave_cycle = (cycle * iSBZL_WAVE_NUM) / iSBZL_WAVE_DEN;

    sbzl_hue_phase  += n;
    sbzl_wave_phase += n;
    while(cycle <= sbzl_hue_phase)                  // 也兼容运行中切换到更短周期
        sbzl_hue_phase -= cycle;
    while(wave_cycle <= sbzl_wave_phase)
        sbzl_wave_phase -= wave_cycle;

    hue  = ((uint32_t)sbzl_hue_phase  * iSBZL_HUE_SPAN) / cycle;
    wpos = ((uint32_t)sbzl_wave_phase * iSBZL_POS_MAX)  / wave_cycle;
    en   = rgb_en_tbl[led_rgb_index&0x07];

    for(i=0; i<iLED_MAX; i++)
    {
        unsigned short r, g, b, h;

        // 灯 i 显示的是灯 i-1 稍早的状态 -> 颜色与涌浪都向高序号流动
        x   = (wpos + iSBZL_POS_MAX - i * (iSBZL_POS_MAX / iLED_MAX)) % iSBZL_POS_MAX;
        lum = sbzl_wave(x);

        if(iRGB_QICAI == (led_rgb_index&0x07))
        {
            h  = (hue + iSBZL_HUE_SPAN - (i * iSBZL_HUE_OFFSET) % iSBZL_HUE_SPAN) % iSBZL_HUE_SPAN;
            h0 = h >> 8;
            hf = h & 0xFF;
            h1 = (iHUE_NUM-1 <= h0) ? 0 : (h0 + 1);
            r  = (hue_rgb_tbl[h0][0] * (256 - hf) + hue_rgb_tbl[h1][0] * hf) >> 8;
            g  = (hue_rgb_tbl[h0][1] * (256 - hf) + hue_rgb_tbl[h1][1] * hf) >> 8;
            b  = (hue_rgb_tbl[h0][2] * (256 - hf) + hue_rgb_tbl[h1][2] * hf) >> 8;
        }
        else
        {
            r = (en & iPWM_R) ? iPWM_MAX : 0;
            g = (en & iPWM_G) ? iPWM_MAX : 0;
            b = (en & iPWM_B) ? iPWM_MAX : 0;
        }

        pwm_rgb_tbl[i][iR_INDEX] = (r * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iG_INDEX] = (g * lum + 2048) >> 12;
        pwm_rgb_tbl[i][iB_INDEX] = (b * lum + 2048) >> 12;
    }
}

//-----------------------------------------------------------------------------;
// 爆闪（BS）
// 画面由“进入后经过的绝对时间”直接算出帧号，不做逐帧累加：
// 无论 logo_process 调用快慢、是否被写 flash/休眠打断，节奏始终与时钟严格一致
//
// 每帧间隔(ms)，下标 = 速度档（0 最快）
static const unsigned short bs_period_ms_tbl[] = { 300, 450, 675, 1050, 1500 };

#define iBS_COLOR_NUM       7                               // 颜色序列：rgb_en_tbl[0..6]
#define iBS_SUB_NUM         4                               // 每色 4 帧：偶数灯正色/奇数灯正色/偶数灯反色/奇数灯反色
#define iBS_FRAME_NUM       (iBS_COLOR_NUM * iBS_SUB_NUM)   // 一轮 28 帧

static uint32_t                 bs_start;                   // 帧号基准时刻
static uint8_t                  bs_base;                    // 基准时刻对应的帧号
static uint8_t                  bs_lev;                     // 基准时刻采用的速度档

/******************************************************************************
* 函数名称: led_mode_baoshan_init
* 功能描述: 爆闪进入初始化（切换灯效时调用），从第 0 帧开始计时
******************************************************************************/
void led_mode_baoshan_init(void)
{
    bs_start = drv_rtc_read(OM_RTC);
    bs_base  = 0;
    bs_lev   = led_speed_index();
}

/******************************************************************************
* 函数名称: led_mode_baoshan
* 功能描述: 爆闪：奇偶灯交替、正反色交替闪烁，7 色循环
* 输入参数: 无
* 输出参数: 无
* 说    明: 帧号 = 基准帧 + (当前时刻 - 基准时刻) / 帧周期；每次调用都按帧号
*           重画，外部清灯（关灯/休眠/回连）恢复后立即回到正确画面
******************************************************************************/
void led_mode_baoshan(void)
{
    uint32_t now, period, k;
    uint8_t  frame, sub, en, j;

    now    = drv_rtc_read(OM_RTC);
    period = mMS_TO_TICK(bs_period_ms_tbl[bs_lev]);
    k      = (now - bs_start) / period;

    // 运行中改速度档：保持当前帧，从此刻起按新周期计时
    if(bs_lev != led_speed_index())
    {
        bs_base  = (bs_base + k) % iBS_FRAME_NUM;
        bs_start = now;
        bs_lev   = led_speed_index();
        k        = 0;
    }

    // 每满一轮把基准时刻前移，保证差值不会溢出
    if(iBS_FRAME_NUM <= k)
    {
        bs_start += (k / iBS_FRAME_NUM) * iBS_FRAME_NUM * period;
        k        %= iBS_FRAME_NUM;
    }

    frame = (bs_base + k) % iBS_FRAME_NUM;
    sub   = frame % iBS_SUB_NUM;

    en = rgb_en_tbl[frame / iBS_SUB_NUM];
    if(2 <= sub)
    {
        en = ~en;                               // 后两帧为反色
        if(0 == (en & iPWM_RGB))
            en = iPWM_R;
    }

    clr_rgb();
    rgb_enable = en;
    for(j = (sub & 0x01); j < iLED_MAX; j += 2) // 偶数帧点亮偶数灯，奇数帧点亮奇数灯
    {
        gtemp_j = j;
        rgb_w_max();
    }
}

// 来回拖尾（LHTW）逐灯颜色使能表
unsigned char       const   lhtw_en_tbl[]={
    iPWM_R,
    iPWM_G,
    iPWM_B,
    iPWM_R|iPWM_B,
    iPWM_R|iPWM_G,
    iPWM_G|iPWM_B,
    iPWM_W,

    iPWM_R,
    iPWM_G,
    iPWM_B,
    iPWM_R|iPWM_B,
    iPWM_R|iPWM_G,
    iPWM_G|iPWM_B,
    iPWM_W,
    iPWM_R,
    iPWM_G,
};

//-----------------------------------------------------------------------------;
// 七彩霓虹（QCNH）
//
// 赤橙黄绿青蓝紫 7 个色块沿灯带流动，像一节一节的霓虹灯管：
//   - 色块中心纯色最亮，交界处色相平滑过渡、亮度下降到约 60%（感知）
//   - 亮度只跟随色块，不再固定在灯位上（原两端灯恒定偏暗）
//   - 色环按色相重排，赤橙黄三色拉开、更易分辨
//
// ---- 可调参数 ----
#define iQCNH_DIR           0                               // 流向：1 = 高序号 -> 低序号，0 = 低序号 -> 高序号（上板确认用 0）
#define iQCNH_LED_PER_COLOR 2                               // 每个色块约占几颗灯（8 颗灯上约 4 个色块）
#define iQCNH_HOLD          96                              // 每块中心纯色区宽度（0~256，其余为过渡）

// 色环转一圈的时间(ms)，下标 = 速度档（0 最快），与现行 4.5/6.7/9.0s 一致
static const unsigned short qcnh_cycle_ms_tbl[] = { 4500, 6700, 9000, 13400, 26900 };

// 七彩色环（R,G,B，满量程 iPWM_MAX）
// 色相 0/28/55/120/180/235/280°：暖色拉开（原赤橙黄挤在一起），黄绿、青蓝间距收窄。
// 未做亮度补偿：按灯珠典型发光强度 R:G:B ≈ 0.29:0.58:0.13 估算，转一圈总亮度波动约 ±37%
// （旧版约 ±30%），平均亮度约为旧版 85%。
// 实测补偿代价过大：k=0.3 部分补偿（因子 (Lmin/L)^0.3）只能把波动拉回约 ±28%，
// 平均亮度却降到旧版约 61%，要压到 ±15% 需降到约 28%。如需补偿版可替换为：
//   {55,0,0},{45,21,0},{40,37,0},{0,45,0},{0,42,42},{0,5,64},{36,0,54}
static const unsigned char qcnh_color_tbl[7][3] = {
    {64,  0,  0},   // 赤   0°
    {64, 30,  0},   // 橙  28°
    {64, 59,  0},   // 黄  55°
    { 0, 64,  0},   // 绿 120°
    { 0, 64, 64},   // 青 180°
    { 0,  5, 64},   // 蓝 235°
    {43,  0, 64},   // 紫 280°
};

// 色块亮度截面（半块，距块中心 0~128 -> 33 点，0~4096）：(1 - 0.4·(1-cos)/2)^2.2
// 块中心 100%，交界处感知亮度 60%（线性约 32%）
static const unsigned short qcnh_block_tbl[33] = {
    4096, 4087, 4061, 4019, 3960, 3886, 3798, 3698, 3587, 3466, 3337,
    3203, 3065, 2925, 2784, 2644, 2507, 2374, 2246, 2124, 2009, 1903,
    1804, 1715, 1634, 1563, 1501, 1449, 1407, 1374, 1350, 1336, 1331,
};

// ---- 内部常量 ----
#define iQCNH_COLOR_NUM     7
#define iQCNH_COLOR_SPAN    256                             // 相邻两色块中心之间的相位细分
#define iQCNH_RING_LEN      (iQCNH_COLOR_NUM * iQCNH_COLOR_SPAN)    // 1792
#define iQCNH_LED_SPACING   (iQCNH_COLOR_SPAN / iQCNH_LED_PER_COLOR)
#define iQCNH_HOLD_LO       (iQCNH_HOLD / 2)                // f <= 此值：仍为当前色
#define iQCNH_HOLD_HI       (iQCNH_COLOR_SPAN - iQCNH_HOLD / 2)     // f >= 此值：已为下一色

static uint16_t                 qcnh_phase;                 // 周期内相位（基准节拍 7.5ms 数）

/******************************************************************************
* 函数名称: led_mode_qcnh_init
* 功能描述: 七彩霓虹进入初始化（切换灯效时调用）
******************************************************************************/
void led_mode_qcnh_init(void)
{
    qcnh_phase = 0;
}

// 色相混合比：f 为距当前色块中心的相位 0~255 -> 下一色占比 0~256
// 两端各 iQCNH_HOLD/2 为纯色，中间 smoothstep(3t²-2t³) 过渡
static uint16_t qcnh_blend(uint8_t f)
{
    uint32_t t;

    if(f <= iQCNH_HOLD_LO)
        return 0;
    if(f >= iQCNH_HOLD_HI)
        return 256;

    t = ((uint32_t)(f - iQCNH_HOLD_LO) * 256) / (iQCNH_HOLD_HI - iQCNH_HOLD_LO);   // 0~255
    return (uint16_t)((t * t * (768 - 2 * t)) >> 16);
}

// 色块亮度：f 为距当前色块中心的相位 0~255 -> 0~4096（交界 f=128 最暗）
static uint16_t qcnh_block(uint8_t f)
{
    uint8_t  x, idx, frac;
    uint16_t a;

    x    = (f <= 128) ? f : (uint8_t)(256 - f);           // 0~128，对称
    idx  = x >> 2;
    frac = x & 0x03;
    a    = qcnh_block_tbl[idx];
    if(32 <= idx)
        return a;
    return a - (((a - qcnh_block_tbl[idx+1]) * frac) >> 2);
}

/******************************************************************************
* 函数名称: led_mode_qcnh
* 功能描述: 七彩霓虹：7 个色块像霓虹灯管一样沿灯带流动
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间推进相位，运行中改速度档不跳变；
*           颜色与亮度以 12bit 精度相乘后一次四舍五入到 0~64
******************************************************************************/
void led_mode_qcnh(void)
{
    uint8_t  i, n, c, nc, f;
    uint16_t cycle, pos, p, t, lum;
    const unsigned char *a, *b;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return;

    cycle = ((uint32_t)qcnh_cycle_ms_tbl[led_speed_index()] * 2) / 15;    // ms -> 7.5ms 节拍数
    qcnh_phase += n;
    while(cycle <= qcnh_phase)
        qcnh_phase -= cycle;

    pos = ((uint32_t)qcnh_phase * iQCNH_RING_LEN) / cycle;

    for(i = 0; i<iLED_MAX; i++)
    {
#if (1 == iQCNH_DIR)
        // 灯 i 显示的是灯 i+1 稍早的颜色 -> 色块从高序号流向低序号
        p = (pos + i * iQCNH_LED_SPACING) % iQCNH_RING_LEN;
#else
        // 灯 i 显示的是灯 i-1 稍早的颜色 -> 色块从低序号流向高序号
        p = (pos + (iLED_MAX - 1 - i) * iQCNH_LED_SPACING) % iQCNH_RING_LEN;
#endif
        c   = p / iQCNH_COLOR_SPAN;
        f   = p % iQCNH_COLOR_SPAN;
        nc  = (iQCNH_COLOR_NUM - 1 <= c) ? 0 : (c + 1);
        a   = qcnh_color_tbl[c];
        b   = qcnh_color_tbl[nc];
        t   = qcnh_blend(f);
        lum = qcnh_block(f);

        // (颜色 0~16384) × (亮度 0~4096) >> 20，四舍五入
        pwm_rgb_tbl[i][iR_INDEX] = ((uint32_t)(a[0] * (256 - t) + b[0] * t) * lum + (1UL << 19)) >> 20;
        pwm_rgb_tbl[i][iG_INDEX] = ((uint32_t)(a[1] * (256 - t) + b[1] * t) * lum + (1UL << 19)) >> 20;
        pwm_rgb_tbl[i][iB_INDEX] = ((uint32_t)(a[2] * (256 - t) + b[2] * t) * lum + (1UL << 19)) >> 20;
    }
}

//-----------------------------------------------------------------------------;
// 拖尾类公共：彗星亮度、按位置取色
//
#define iTW_TAIL            4                               // 彗星长度（灯数，含头）
#define iTW_PAUSE           256                             // 每轮开始前的空档（1/256 灯，1 灯时间）

// 彗星尾部亮度（距头 0 ~ iTW_TAIL 灯 -> 33 点，0~4096）：(1-x)^2.2
// 感知亮度依次约 100% / 75% / 50% / 25%，头尾分明
static const unsigned short tw_tail_tbl[33] = {
    4096, 3820, 3554, 3298, 3053, 2819, 2594, 2380, 2175, 1981, 1796,
    1621, 1456, 1301, 1155, 1019,  891,  773,  665,  565,  473,  391,
     317,  251,  194,  145,  103,   69,   42,   22,    9,    2,    0,
};

static uint16_t                 tw_phase;                   // 周期内相位（基准节拍数）

/******************************************************************************
* 函数名称: led_mode_tw_init
* 功能描述: 拖尾类灯效进入初始化（切换灯效时调用）
******************************************************************************/
void led_mode_tw_init(void)
{
    tw_phase = 0;
}

// d：灯在头后方的距离（1/256 灯，负值为头前方）-> 亮度 0~4096
// 头前方半颗灯线性渐亮，头部亚像素平滑移动，不再一格一格跳
static uint16_t tw_comet(int16_t d)
{
    uint16_t x, a;
    uint8_t  idx, frac;

    if(d <= -128)
        return 0;
    if(d < 0)
        return (uint16_t)(d + 128) << 5;                    // 0~4096
    if((iTW_TAIL * 256) <= d)
        return 0;

    x    = ((uint32_t)d * 1024) / (iTW_TAIL * 256);         // 0~1023
    idx  = x >> 5;
    frac = x & 0x1F;
    a    = tw_tail_tbl[idx];
    return a - (((a - tw_tail_tbl[idx+1]) * frac) >> 5);
}

// 按位置取色并输出：七彩时取色环 hue(0~47)，否则按 led_rgb_index 所选单色
static void tw_put(uint8_t i, uint8_t hue, uint16_t lum)
{
    uint16_t r, g, b;
    uint8_t  en;

    if(iRGB_QICAI == (led_rgb_index&0x07))
    {
        r = hue_rgb_tbl[hue][0];
        g = hue_rgb_tbl[hue][1];
        b = hue_rgb_tbl[hue][2];
    }
    else
    {
        en = rgb_en_tbl[led_rgb_index&0x07];
        r = (en & iPWM_R) ? iPWM_MAX : 0;
        g = (en & iPWM_G) ? iPWM_MAX : 0;
        b = (en & iPWM_B) ? iPWM_MAX : 0;
    }

    pwm_rgb_tbl[i][iR_INDEX] = ((uint32_t)r * lum + 2048) >> 12;
    pwm_rgb_tbl[i][iG_INDEX] = ((uint32_t)g * lum + 2048) >> 12;
    pwm_rgb_tbl[i][iB_INDEX] = ((uint32_t)b * lum + 2048) >> 12;
}

// 推进相位并返回周期内相位；tpl = 移动一颗灯的节拍数，len = 一轮长度（灯数，可为 1/256 精度）
// 返回 0xFFFF 表示本次未到时间
static uint16_t tw_advance(uint16_t tpl, uint32_t len256)
{
    uint8_t  n;
    uint16_t cycle;

    n = led_timer_take(&led_timer_main, iLED_BASE_TICK, iLED_CATCHUP_MAX);
    if(0 == n)
        return 0xFFFF;

    cycle = (uint16_t)((len256 * tpl) >> 8);
    tw_phase += n;
    while(cycle <= tw_phase)
        tw_phase -= cycle;
    return tw_phase;
}

// 相位 -> 头位置（1/256 灯），从 -iTW_PAUSE-128 起步（先空档，再从第 0 颗灯外侧进入）
static int16_t tw_head(uint16_t phase, uint16_t tpl)
{
    return (int16_t)(((uint32_t)phase * 256) / tpl) - iTW_PAUSE - 128;
}

// 移动一颗灯的节拍数：17 × (速度档+1)，即 127.5 / 255 / 382.5ms，与原速度一致
// （原实现头部亮度从 64 减到 48 以下才前进一格，共 17 步）
#define mTW_TPL()           ((uint16_t)17 * (led_speed_index() + 1))

// 单程长度：空档 + 进入半颗 + 8 颗灯 + 尾巴完全离场，约 13 颗灯（原单向拖尾一轮 13 格）
#define iTW_RUN_LEN256      (iTW_PAUSE + 128 + iLED_MAX * 256 + iTW_TAIL * 256 - 128)

/******************************************************************************
* 函数名称: led_mode_dxtw
* 功能描述: 单向拖尾：彗星从低序号灯平滑划向高序号灯，循环
******************************************************************************/
void led_mode_dxtw(void)
{
    uint8_t  i;
    uint16_t tpl = mTW_TPL(), ph;
    int16_t  h;

    ph = tw_advance(tpl, iTW_RUN_LEN256);
    if(0xFFFF == ph)
        return;

    h = tw_head(ph, tpl);
    for(i=0; i<iLED_MAX; i++)
        tw_put(i, (i * (iHUE_NUM/iLED_MAX)) % iHUE_NUM, tw_comet(h - (int16_t)i * 256));
}

/******************************************************************************
* 函数名称: led_mode_sxtw
* 功能描述: 双向拖尾：彗星划到高序号端离场后，再从高序号端反向划回，往复
******************************************************************************/
void led_mode_sxtw(void)
{
    uint8_t  i, back;
    uint16_t tpl = mTW_TPL(), ph, leg;
    int16_t  h;

    ph = tw_advance(tpl, 2UL * iTW_RUN_LEN256);
    if(0xFFFF == ph)
        return;

    leg  = (uint16_t)(((uint32_t)iTW_RUN_LEN256 * tpl) >> 8);
    back = (leg <= ph);
    if(back)
        ph -= leg;

    h = tw_head(ph, tpl);
    for(i=0; i<iLED_MAX; i++)
    {
        uint8_t pos = back ? (iLED_MAX - 1 - i) : i;        // 回程镜像：头朝低序号、尾在高序号侧
        tw_put(i, (i * (iHUE_NUM/iLED_MAX)) % iHUE_NUM, tw_comet(h - (int16_t)pos * 256));
    }
}

/******************************************************************************
* 函数名称: led_mode_txtw
* 功能描述: 对向拖尾（两端 -> 中间）/ 反向拖尾（中间 -> 两端）
* 说    明: 两颗彗星镜像对称；灯 i 与灯 7-i 为一对，同色
******************************************************************************/
void led_mode_txtw(void)
{
    uint8_t  i, pair;
    uint16_t tpl = mTW_TPL(), ph;
    int16_t  h;

    // 半边 4 颗灯：空档 + 4 颗 + 尾巴离场，一轮 8 格（与原一致，尾巴末端已接近全暗）
    ph = tw_advance(tpl, iTW_PAUSE + (iLED_MAX/2) * 256 + iTW_TAIL * 256 - 256);
    if(0xFFFF == ph)
        return;

    h = tw_head(ph, tpl);
    for(i=0; i<iLED_MAX; i++)
    {
        pair = (i < iLED_MAX/2) ? i : (iLED_MAX - 1 - i);   // 0 = 两端，3 = 中间
        if(iMODE_LED_FXTW == led_cur_mode)
            pair = (iLED_MAX/2 - 1) - pair;                 // 反向：中间先亮
        tw_put(i, (((i < iLED_MAX/2) ? i : (iLED_MAX - 1 - i)) * 12) % iHUE_NUM,
               tw_comet(h - (int16_t)pair * 256));
    }
}

//-----------------------------------------------------------------------------;
// 来回拖尾：灯条从低序号逐颗亮满，再从高序号逐颗熄灭，往复
//
// 每格时间(基准节拍数)，下标 = 速度档：约 60 / 90 / 135ms，与原速度一致
static const unsigned char lhtw_step_tbl[] = { 8, 12, 18, 24, 32 };

/******************************************************************************
* 函数名称: led_mode_lhtw_init
* 功能描述: 来回拖尾进入初始化（切换灯效时调用）
******************************************************************************/
void led_mode_lhtw_init(void)
{
    tw_phase = 0;
}

/******************************************************************************
* 函数名称: led_mode_lhtw
* 功能描述: 来回拖尾：灯条亮满 -> 停 1 格 -> 熄灭 -> 停 2 格，每颗灯渐亮渐灭
* 说    明: 一轮 19 格，与原一致
******************************************************************************/
void led_mode_lhtw(void)
{
    uint8_t  i;
    uint16_t tpl = lhtw_step_tbl[led_speed_index()], ph;
    int32_t  lv, x;

    ph = tw_advance(tpl, (2 * iLED_MAX + 3) * 256);
    if(0xFFFF == ph)
        return;

    lv = ((uint32_t)ph * 256) / tpl;                        // 0 ~ 19×256
    if(lv < iLED_MAX * 256)
        ;                                                   // 亮起中：灯条长度 = lv
    else if(lv < (iLED_MAX + 1) * 256)
        lv = iLED_MAX * 256;                                // 全亮停 1 格
    else if(lv < (2 * iLED_MAX + 1) * 256)
        lv = (2 * iLED_MAX + 1) * 256 - lv;                 // 熄灭中
    else
        lv = 0;                                             // 全灭停 2 格

    for(i=0; i<iLED_MAX; i++)
    {
        x = lv - (int32_t)i * 256;                          // 本颗灯的点亮进度
        if(x < 0)   x = 0;
        if(x > 256) x = 256;
        tw_put(i, (i * (iHUE_NUM/iLED_MAX)) % iHUE_NUM, (uint16_t)((x * x) >> 4));   // ≈γ2
    }
}
