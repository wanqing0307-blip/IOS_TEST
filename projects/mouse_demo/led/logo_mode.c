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
static uint16_t                qcnh_phase;          // 七彩霓虹色环相位（8.8 定点：高字节=色号，低字节=插值）

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

// 通用连续灯效：每 (速度档+1) 个基准节拍走一步，与原“每 (led_speed_lev+1) 次调用走一步”一致
static uint8_t led_speed_steps(void)
{
    return led_timer_take(&led_timer_main, (led_speed_index() + 1) * iLED_BASE_TICK, iLED_CATCHUP_MAX);
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
        if(iRGB_QICAI == led_rgb_index)
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

/******************************************************************************
* 函数名称: led_mode_snake
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/

/******************************************************************************
* 函数名称: led_mode_snake
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
static void led_mode_nhjb_step(void)
{
    unsigned char i;


    if(0 == led_process_index)
    {
        pwm_duty_array_0[1]++;
        if(iPWM_MAX <= pwm_duty_array_0[1])
            led_process_index++;
    }
    else if(1 == led_process_index)
    {
        pwm_duty_array_0[0]--;
        if(0 == pwm_duty_array_0[0])
            led_process_index++;
    }
    else if(2 == led_process_index)
    {
        pwm_duty_array_0[2]++;
        if(iPWM_MAX <= pwm_duty_array_0[2])
            led_process_index++;
    }
    else if(3 == led_process_index)
    {
        pwm_duty_array_0[1]--;
        if(0 == pwm_duty_array_0[1])
            led_process_index++;
    }
    else if(4 == led_process_index)
    {
        pwm_duty_array_0[0]++;
        if(iPWM_MAX <= pwm_duty_array_0[0])
            led_process_index++;
    }
    else //if(5 == led_process_index)
    {
        pwm_duty_array_0[2]--;
        if(0 == pwm_duty_array_0[2])
            led_process_index = 0;
    }

    for(i=0; i<iLED_MAX; i++)
    {
        pwm_rgb_tbl[i][0] = pwm_duty_array_0[iR_INDEX];
        pwm_rgb_tbl[i][1] = pwm_duty_array_0[iG_INDEX];
        pwm_rgb_tbl[i][2] = pwm_duty_array_0[iB_INDEX];
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
#define iQCNH_COLOR_NUM     7                                   // 赤橙黄绿青蓝紫
#define iQCNH_COLOR_SPAN    256                                 // 相邻两色之间的插值细分
#define iQCNH_RING_LEN      (iQCNH_COLOR_NUM * iQCNH_COLOR_SPAN)// 色环总长 1792
#define iQCNH_LED_SPACING   (iQCNH_RING_LEN / 12)               // 相邻灯相位差约 30°：8 颗灯覆盖约 2/3 色环，
                                                                // 整体颜色随时间明显变换（不再整条铺满一圈）
#define iQCNH_HOLD          96                                  // 每色纯色停留占比 96/256，其余为过渡 → 色带分明

// 七彩色环（R,G,B，满量程 iPWM_MAX），首尾相接循环插值
static const unsigned char qcnh_color_tbl[iQCNH_COLOR_NUM][3] = {
    {64,  0,  0},   // 赤
    {64, 20,  0},   // 橙
    {64, 56,  0},   // 黄
    { 0, 64,  0},   // 绿
    { 0, 64, 48},   // 青
    { 0,  0, 64},   // 蓝
    {40,  0, 64},   // 紫
};

// 每 15ms 相位步进，下标 = 速度档（0 最快）
// 一圈周期约：6.7s / 9.0s / 13.4s / 26.9s / 26.9s
static const unsigned char qcnh_step_tbl[] = { 4, 3, 2, 1, 1 };
#define iQCNH_TICK          mMS_TO_TICK(15)

// 固定亮度轮廓：中间亮、两端稍暗，提供稳定的空间层次
static const unsigned char qcnh_layer_tbl[iLED_MAX] = { 36, 48, 58, 64, 64, 58, 48, 36 };

// 色带整形：前 iQCNH_HOLD 保持纯色，其后 smoothstep 平滑过渡到下一色
// 输入/输出 0~255
static unsigned char qcnh_band(unsigned char f)
{
    uint32_t t;

    if(f < iQCNH_HOLD)
        return 0;

    t = ((uint32_t)(f - iQCNH_HOLD) * 256) / (256 - iQCNH_HOLD);   // 0~255
    return (unsigned char)((t * t * (768 - 2 * t)) >> 16);          // 3t²-2t³
}

/******************************************************************************
* 函数名称: led_mode_qcnh_init
* 功能描述: 七彩霓虹进入初始化（切换灯效时调用）
* 输入参数: 无
* 输出参数: 无
******************************************************************************/
void led_mode_qcnh_init(void)
{
    qcnh_phase = 0;
}

/******************************************************************************
* 函数名称: led_mode_qcnh
* 功能描述: 七彩霓虹：赤橙黄绿青蓝紫色带在灯带上平滑流动，整体颜色持续变换
* 输入参数: 无
* 输出参数: 无
* 说    明: 按 RTC 时间每 15ms 推进相位；相位 8.8 定点，相邻两色线性插值，无跳变
******************************************************************************/
void led_mode_qcnh(void)
{
    unsigned char  i, c, n, frac, light, steps;
    unsigned short pos;
    const unsigned char *a, *b;

    steps = led_timer_take(&led_timer_main, iQCNH_TICK, iLED_CATCHUP_MAX);
    if(0 == steps)
        return;

    qcnh_phase += (unsigned short)qcnh_step_tbl[led_speed_index()] * steps;
    while(iQCNH_RING_LEN <= qcnh_phase)
        qcnh_phase -= iQCNH_RING_LEN;

    for(i = 0; i<iLED_MAX; i++)
    {
        // 方向反向（配合灯珠反向接线）：颜色从低序号灯流向高序号灯
        pos = qcnh_phase + (iLED_MAX - i) * iQCNH_LED_SPACING;
        while(iQCNH_RING_LEN <= pos)
            pos -= iQCNH_RING_LEN;

        c    = pos / iQCNH_COLOR_SPAN;
        frac = qcnh_band(pos % iQCNH_COLOR_SPAN);
        n    = (iQCNH_COLOR_NUM-1 <= c) ? 0 : (c + 1);
        a    = qcnh_color_tbl[c];
        b    = qcnh_color_tbl[n];
        light = qcnh_layer_tbl[i];

        // 先插值得到颜色，再乘亮度轮廓（>>6 对应满量程 64）
        pwm_rgb_tbl[i][iR_INDEX] = ((((a[0] * (256 - frac)) + (b[0] * frac)) >> 8) * light) >> 6;
        pwm_rgb_tbl[i][iG_INDEX] = ((((a[1] * (256 - frac)) + (b[1] * frac)) >> 8) * light) >> 6;
        pwm_rgb_tbl[i][iB_INDEX] = ((((a[2] * (256 - frac)) + (b[2] * frac)) >> 8) * light) >> 6;
    }
}

// 来回拖尾：每步间隔(基准节拍数)，下标 = 速度档
static const unsigned char lhtw_step_tbl[] = { 8, 12, 18, 24, 32 };
/******************************************************************************
* 函数名称: led_mode_lhtw
* 功能描述: 来回拖尾
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
void led_mode_lhtw(void)
{
    unsigned char i;

    if(0 == led_timer_take(&led_timer_main, lhtw_step_tbl[led_speed_index()] * iLED_BASE_TICK, 1))
        return;                                 // 离散跳帧，不补帧

    if(!bback_led_status)
    {
        for(i=0; i<led_process_index; i++)
        {
            rgb_enable = lhtw_en_tbl[i];
	        gtemp_j = i;
	        rgb_w_max();
        }
        led_process_index++;
        if(iLED_MAX < led_process_index)
            bback_led_status = 1;
    }
    else
    {
        for(i=led_process_index; i<iLED_MAX; i++)
        {
            rgb_enable = lhtw_en_tbl[i];
            pwm_rgb_tbl[i][iR_INDEX] = 0;
            pwm_rgb_tbl[i][iG_INDEX] = 0;
            pwm_rgb_tbl[i][iB_INDEX] = 0;
        }

        if(0 != led_process_index)
            led_process_index--;
        else
            bback_led_status = 0;
    }
}

/******************************************************************************
* 函数名称: led_mode_dxtw
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
static void led_mode_dxtw_step(void)
{
    unsigned char x;


	snake_led_index &= 0x0F;
    x = --pwm_duty_array_0[snake_led_index];
    if(((iPWM_SNAKE_MAX*3)/4)>x)
    {
    	snake_led_index++;
        if(iLED_DXTW<=snake_led_index)
        {
            snake_led_index = 3;
            pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
            return;
        }
        pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
    }
    x = pwm_duty_array_0[snake_led_index];
    pwm_duty_array_0[snake_led_index-1] = x-((iPWM_SNAKE_MAX*1)/4);
    pwm_duty_array_0[snake_led_index-2] = x-((iPWM_SNAKE_MAX*2)/4);
    pwm_duty_array_0[snake_led_index-3] = x-((iPWM_SNAKE_MAX*3)/4);

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
    for(gtemp_x=5; gtemp_x<(iLED_MAX+5); gtemp_x++)
    {
        //if(iRGB_QICAI == led_rgb_index)
        {
            rgb_enable = (gtemp_x&0x07);
            if(0==rgb_enable)
                rgb_enable = 4;
        }

        gtemp_j = (gtemp_x-5);
        rgb_pwm0_w();
    }
}

/******************************************************************************
* 函数名称: led_mode_dxtw
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
static void led_mode_sxtw_step(void)
{
    unsigned char x;


	snake_led_index &= 0x0F;
    if(!bback_led_status)
    {
        x = --pwm_duty_array_0[snake_led_index];
        if(((iPWM_SNAKE_MAX*3)/4)>x)
        {
        	snake_led_index++;
            if(iLED_DXTW<=snake_led_index)
            {
                snake_led_index = (iLED_DXTW-4);
                pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
                bback_led_status = 1;
                return;
            }
            pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
        }
        x = pwm_duty_array_0[snake_led_index];
        pwm_duty_array_0[snake_led_index-1] = x-((iPWM_SNAKE_MAX*1)/4);
        pwm_duty_array_0[snake_led_index-2] = x-((iPWM_SNAKE_MAX*2)/4);
        pwm_duty_array_0[snake_led_index-3] = x-((iPWM_SNAKE_MAX*3)/4);
    }
    else
    {
        x = --pwm_duty_array_0[snake_led_index];
        if(((iPWM_SNAKE_MAX*3)/4)>x)
        {
            if(0 != snake_led_index)
            {
        	    snake_led_index--;
            }
            else
            {
                snake_led_index = 3;
                pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
                bback_led_status = 0;
                return;
            }
            pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
        }
        x = pwm_duty_array_0[snake_led_index];
        pwm_duty_array_0[snake_led_index+1] = x-((iPWM_SNAKE_MAX*1)/4);
        pwm_duty_array_0[snake_led_index+2] = x-((iPWM_SNAKE_MAX*2)/4);
        pwm_duty_array_0[snake_led_index+3] = x-((iPWM_SNAKE_MAX*3)/4);
    }

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
    for(gtemp_x=5; gtemp_x<(iLED_MAX+5); gtemp_x++)
    {
        //if(iRGB_QICAI == led_rgb_index)
        {
            rgb_enable = (gtemp_x&0x07);
            if(0==rgb_enable)
                rgb_enable = 4;
        }

        gtemp_j = (gtemp_x-5);
        rgb_pwm0_w();
    }
}
/******************************************************************************
* 函数名称: led_mode_dxtw
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
static void led_mode_txtw_step(void)
{
    unsigned char x;


	snake_led_index &= 0x0F;
    x = --pwm_duty_array_0[snake_led_index];
    if(((iPWM_SNAKE_MAX*3)/4)>x)
    {
    	snake_led_index++;
        if(iLED_TXTW<=snake_led_index)
        {
            snake_led_index = 3;
            pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
            return;
        }
        pwm_duty_array_0[snake_led_index] = iPWM_SNAKE_MAX;
    }
    x = pwm_duty_array_0[snake_led_index];
    pwm_duty_array_0[snake_led_index-1] = x-((iPWM_SNAKE_MAX*1)/4);
    pwm_duty_array_0[snake_led_index-2] = x-((iPWM_SNAKE_MAX*2)/4);
    pwm_duty_array_0[snake_led_index-3] = x-((iPWM_SNAKE_MAX*3)/4);

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];

    if(iMODE_LED_TXTW == led_cur_mode)
    {
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x01;
        gtemp_x = 4;
        gtemp_j = 0;
        rgb_pwm0_w();
        gtemp_j = 7;
        rgb_pwm0_w();
    
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x02;
        gtemp_x = 5;
        gtemp_j = 1;
        rgb_pwm0_w();
        gtemp_j = 6;
        rgb_pwm0_w();
    
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x04;
        gtemp_x = 6;
        gtemp_j = 2;
        rgb_pwm0_w();
        gtemp_j = 5;
        rgb_pwm0_w();
    
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x03;
        gtemp_x = 7;
        gtemp_j = 3;
        rgb_pwm0_w();
        gtemp_j = 4;
        rgb_pwm0_w();
    }
    else if(iMODE_LED_FXTW == led_cur_mode)
    {
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x01;
        gtemp_x = 7;
        gtemp_j = 0;
        rgb_pwm0_w();
        gtemp_j = 7;
        rgb_pwm0_w();
    
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x02;
        gtemp_x = 6;
        gtemp_j = 1;
        rgb_pwm0_w();
        gtemp_j = 6;
        rgb_pwm0_w();
    
        //if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x04;
        gtemp_x = 5;
        gtemp_j = 2;
        rgb_pwm0_w();
        gtemp_j = 5;
        rgb_pwm0_w();
    
       // if(iRGB_QICAI == led_rgb_index)
            rgb_enable = 0x03;
        gtemp_x = 4;
        gtemp_j = 3;
        rgb_pwm0_w();
        gtemp_j = 4;
        rgb_pwm0_w();
    }
}

//=============================================================================;
// 灯效对外入口：按 RTC 时间取出到期步数，逐步执行
//=============================================================================;

void led_mode_nhjb(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_nhjb_step();
}

void led_mode_dxtw(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_dxtw_step();
}

void led_mode_sxtw(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_sxtw_step();
}

void led_mode_txtw(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_txtw_step();
}
