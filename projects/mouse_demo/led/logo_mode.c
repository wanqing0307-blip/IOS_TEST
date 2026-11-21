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

#define iLFSR_MULTINOMIAL       0xF521

bool                            bled_run_dir;

uint16_t                        lfsr_value;
uint8_t                         snake_led_index;
static uint16_t                qcnh_phase;          // 七彩霓虹色环相位（8.8 定点：高字节=色号，低字节=插值）

//-----------------------------------------------------------------------------;
// 表格定义区
//

#define iHUE_NUM        48

// 呼吸用 gamma 表（0~64 -> 0~64，gamma≈2.2，人眼感知线性）
static const unsigned char gamma_tbl[65] = {
     0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  3,
     3,  3,  4,  4,  5,  6,  6,  7,  7,  8,  9, 10, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 27, 28, 29, 31, 32,
    34, 36, 37, 39, 41, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62,
    64,
};

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
static led_timer_t              led_timer_aux;              // 灯效辅助节拍（繁星衰减）

/******************************************************************************
* 函数名称: led_timer_reset
* 功能描述: 灯效时间基准复位（切换灯效时调用），首步立即执行
******************************************************************************/
void led_timer_reset(void)
{
    led_timer_main.refresh = true;
    led_timer_aux.refresh  = true;
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

/******************************************************************************
* 函数名称: led_hint_process
* 功能描述:
* 输入参数:
* 输出参数:
* 说    明:
******************************************************************************/
static void led_mode_breath_step(void)
{
    unsigned char i, h;


    i = pwm_time_count;
    if(!bback_led_status)
    {
        if(pwm_time_count >= iPWM_MAX)
            pwm_value = 0;
        else
            pwm_value = gamma_tbl[iPWM_MAX - i];    // gamma 曲线，呼吸更柔和
    }
    else
    {
        if(pwm_time_count >= iPWM_MAX)
            pwm_value = iPWM_MAX;
        else
            pwm_value = gamma_tbl[i];
    }

    if(pwm_time_count == iPWM_MAX)   // 最亮和最暗的时间加长
    {
        pwm_time_count = 0;
        bback_led_status = !bback_led_status;
        if(bback_led_status)
            rgb_index++;
    }

    // 色环：8 颗灯色相依次相差 45°，随呼吸周期缓慢旋转
    for(i=0; i<iLED_MAX; i++)
    {
        h = (rgb_index*3 + i*(iHUE_NUM/iLED_MAX)) % iHUE_NUM;

        pwm_rgb_tbl[i][iR_INDEX] = (hue_rgb_tbl[h][0] * pwm_value) >> 6;
        pwm_rgb_tbl[i][iG_INDEX] = (hue_rgb_tbl[h][1] * pwm_value) >> 6;
        pwm_rgb_tbl[i][iB_INDEX] = (hue_rgb_tbl[h][2] * pwm_value) >> 6;
    }

    pwm_time_count++;
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

static void led_mode_liuguang_step(void)
{
    unsigned char i, k;
    uint16_t x;


    x = 0x01;
    for(i=0; i<iLED_MAX; i++)
    {
        if(led_status_reg & x)
        {
            pwm_duty_array_0[i]++;
            if(iPWM_MAX <= pwm_duty_array_0[i])
            {
                led_status_reg &= ~x;
            }
        }
        else
        {
            pwm_duty_array_0[i]--;
            if(0 == pwm_duty_array_0[i])
            {
                pwm_duty_array_1[i]++;
                if(7 < pwm_duty_array_1[i])
                    pwm_duty_array_1[i]=1;
                led_status_reg |= x;
            }
        }
        x <<= 1;
    }

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
    rgb_enable = iPWM_RGB;

    for(k=0; k<iLED_MAX; k++)
    {
        if(bled_run_dir)
        {
            gtemp_j = pwm_duty_array_0[k];
            if(iRGB_QICAI == led_rgb_index)
                rgb_enable = pwm_duty_array_1[k];
        }
        else
        {
            gtemp_j = pwm_duty_array_0[iLED_MAX-1-k];
            if(iRGB_QICAI == led_rgb_index)
                rgb_enable = pwm_duty_array_1[iLED_MAX-1-k];
        }

        if(rgb_enable&iPWM_R)
            pwm_rgb_tbl[k][iR_INDEX] = gtemp_j;
        else
            pwm_rgb_tbl[k][iR_INDEX] = 0;
        if(rgb_enable&iPWM_G)
            pwm_rgb_tbl[k][iG_INDEX] = gtemp_j;
        else
            pwm_rgb_tbl[k][iG_INDEX] = 0;
        if(rgb_enable&iPWM_B)
            pwm_rgb_tbl[k][iB_INDEX] = gtemp_j;
        else
            pwm_rgb_tbl[k][iB_INDEX] = 0;
    }
}

// 繁星：新星出现间隔(基准节拍数)，下标 = 速度档
static const unsigned char star_spawn_tbl[] = { 20, 40, 70, 110, 160 };

/******************************************************************************
* 函数名称: led_mode_star_decay
* 功能描述: 繁星：所有星点亮度衰减一级并刷新输出
******************************************************************************/
static void led_mode_star_decay(void)
{
    unsigned char i;

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
    for(i=0; i<iLED_MAX; i++)
    {
        if(0 != pwm_duty_array_0[i])
        {
            pwm_duty_array_0[i]--;

            //if(iRGB_QICAI <= led_rgb_index)
                rgb_enable = (pwm_duty_array_1[i]&0x0F);

            if(rgb_enable&iPWM_R)
                pwm_rgb_tbl[i][iR_INDEX] = pwm_duty_array_0[i];
            else
                pwm_rgb_tbl[i][iR_INDEX] = 0;
            if(rgb_enable&iPWM_G)
                pwm_rgb_tbl[i][iG_INDEX] = pwm_duty_array_0[i];
            else
                pwm_rgb_tbl[i][iG_INDEX] = 0;
            if(rgb_enable&iPWM_B)
                pwm_rgb_tbl[i][iB_INDEX] = pwm_duty_array_0[i];
            else
                pwm_rgb_tbl[i][iB_INDEX] = 0;          
        }
        else
        {
            pwm_rgb_tbl[i][iR_INDEX] = 0;
            pwm_rgb_tbl[i][iG_INDEX] = 0;
            pwm_rgb_tbl[i][iB_INDEX] = 0;
        }
    }
}

/******************************************************************************
* 函数名称: led_mode_star_spawn
* 功能描述: 繁星：随机点亮一颗新星（LFSR 伪随机位置和颜色）
******************************************************************************/
static void led_mode_star_spawn(void)
{
    unsigned char j;

    lfsr_value = lfsr_value>>1;
    if(lfsr_value & 0x01)
        lfsr_value ^= iLFSR_MULTINOMIAL;

    j = ((lfsr_value+(lfsr_value>>8))&0x0F);

    pwm_duty_array_0[j] = iPWM_MAX;
    pwm_duty_array_1[j] = ((lfsr_value>>8)&0x0F);
    if(0==pwm_duty_array_1[j])
        pwm_duty_array_1[j] = 1;
}

/******************************************************************************
* 函数名称: led_mode_snake
* 功能描述:
* 输入参数: 无
* 输出参数: 无
* 说    明:
******************************************************************************/
static void led_mode_sbzl_step(void)
{

    for(gtemp_x=0; gtemp_x<iLED_MAX; gtemp_x++)
    {
        if(0 == pwm_duty_array_3[gtemp_x])
        {
            pwm_duty_array_0[gtemp_x]--;
            pwm_duty_array_1[gtemp_x]=iPWM_MAX-pwm_duty_array_0[gtemp_x];
            if(0 == pwm_duty_array_0[gtemp_x])
                pwm_duty_array_3[gtemp_x]++;
        }
        else if(1 == pwm_duty_array_3[gtemp_x])
        {
            pwm_duty_array_1[gtemp_x]--;
            pwm_duty_array_2[gtemp_x]=iPWM_MAX-pwm_duty_array_1[gtemp_x];
            if(0 == pwm_duty_array_1[gtemp_x])
                pwm_duty_array_3[gtemp_x]++;
        }
        else //if(2 == pwm_duty_array_3[gtemp_x])
        {
            pwm_duty_array_2[gtemp_x]--;
            pwm_duty_array_0[gtemp_x]=iPWM_MAX-pwm_duty_array_2[gtemp_x];
            if(0 == pwm_duty_array_2[gtemp_x])
                pwm_duty_array_3[gtemp_x]=0;
        }
    }

    rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
    rgb_enable = iPWM_RGB;

    if(!bled_run_dir)
    {
        for(gtemp_x = 0; gtemp_x<iLED_MAX; gtemp_x++)
        {
            if(rgb_enable&iPWM_R)
               pwm_rgb_tbl[gtemp_x][iR_INDEX] = pwm_duty_array_0[gtemp_x];
           else
               pwm_rgb_tbl[gtemp_x][iR_INDEX] = 0;
            if(rgb_enable&iPWM_G)
               pwm_rgb_tbl[gtemp_x][iG_INDEX] = pwm_duty_array_1[gtemp_x];
           else
               pwm_rgb_tbl[gtemp_x][iG_INDEX] = 0;
            if(rgb_enable&iPWM_B)
               pwm_rgb_tbl[gtemp_x][iB_INDEX] = pwm_duty_array_2[gtemp_x];
           else
               pwm_rgb_tbl[gtemp_x][iB_INDEX] = 0;
        }
    }
    else
    {
        for(gtemp_x = 0; gtemp_x<iLED_MAX; gtemp_x++)
        {
            if(rgb_enable&iPWM_R)
                pwm_rgb_tbl[gtemp_x][iR_INDEX] = pwm_duty_array_0[iLED_MAX-1-gtemp_x];
            else
                pwm_rgb_tbl[gtemp_x][iR_INDEX] = 0;
            if(rgb_enable&iPWM_G)
                pwm_rgb_tbl[gtemp_x][iG_INDEX] = pwm_duty_array_1[iLED_MAX-1-gtemp_x];
            else
                pwm_rgb_tbl[gtemp_x][iG_INDEX] = 0;
            if(rgb_enable&iPWM_B)
                pwm_rgb_tbl[gtemp_x][iB_INDEX] = pwm_duty_array_2[iLED_MAX-1-gtemp_x];
            else
                pwm_rgb_tbl[gtemp_x][iB_INDEX] = 0;    
        }
    }
}

// 每帧间隔(ms)，下标 = 速度档（0 最快），与原 7.5ms 节拍下的 40/60/90/140/200 拍一致
static const unsigned short bs_period_ms_tbl[] = { 300, 450, 675, 1050, 1500 };

/******************************************************************************
* 函数名称: led_mode_baoshan
* 功能描述: 爆闪：奇偶灯交替、正反色交替闪烁
* 输入参数: 无
* 输出参数: 无
* 说    明: 进入即出首帧，之后按 RTC 时间固定节奏；离散跳帧，不补帧
******************************************************************************/
void led_mode_baoshan(void)
{
    if(0 == led_timer_take(&led_timer_main, mMS_TO_TICK(bs_period_ms_tbl[led_speed_index()]), 1))
        return;

    clr_rgb();

	if(0 == led_process_index)
	{
//	    if(iRGB_QICAI != led_rgb_index)
//            rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
//        else
            rgb_enable = rgb_en_tbl[rgb_index&0x0F];
	    gtemp_j = 0;
	    rgb_w_max();
	    gtemp_j = 2;
	    rgb_w_max();
	    gtemp_j = 4;
	    rgb_w_max();
	    gtemp_j = 6;
	    rgb_w_max();

        led_process_index++;
    }
    else if(1 == led_process_index)
	{
//	    if(iRGB_QICAI != led_rgb_index)
//            rgb_enable = rgb_en_tbl[led_rgb_index&0x0F];
//        else
            rgb_enable = rgb_en_tbl[rgb_index&0x0F];
	    gtemp_j = 1;
	    rgb_w_max();
	    gtemp_j = 3;
	    rgb_w_max();
	    gtemp_j = 5;
	    rgb_w_max();
	    gtemp_j = 7;
	    rgb_w_max();

        led_process_index++;
    }
	else if(2 == led_process_index)
	{
	    //if(iRGB_QICAI != led_rgb_index)
        //    rgb_enable = ~rgb_en_tbl[led_rgb_index&0x0F];
        //else
            rgb_enable = ~rgb_en_tbl[rgb_index&0x0F];
        if(0==(rgb_enable&0x07))
            rgb_enable = 0x01;

	    gtemp_j = 0;
	    rgb_w_max();
	    gtemp_j = 2;
	    rgb_w_max();
	    gtemp_j = 4;
	    rgb_w_max();
	    gtemp_j = 6;
	    rgb_w_max();

        led_process_index++;
    }
    else //if(3 == led_process_index)
	{
//	    if(iRGB_QICAI != led_rgb_index)
//            rgb_enable = ~rgb_en_tbl[led_rgb_index&0x0F];
//        else
            rgb_enable = ~rgb_en_tbl[rgb_index&0x0F];
        if(0==(rgb_enable&0x07))
            rgb_enable = 0x01;
	    gtemp_j = 1;
	    rgb_w_max();
	    gtemp_j = 3;
	    rgb_w_max();
	    gtemp_j = 5;
	    rgb_w_max();
	    gtemp_j = 7;
	    rgb_w_max();

        led_process_index=0;
        rgb_index++;
        if(7<=rgb_index)
            rgb_index = 0;
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
#define iQCNH_LED_SPACING   (iQCNH_RING_LEN / iLED_MAX)         // 相邻灯相位差：整条灯带正好铺满一圈七彩

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
* 功能描述: 七彩霓虹：赤橙黄绿青蓝紫色环在灯带上连续平滑流动
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
        frac = pos % iQCNH_COLOR_SPAN;
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

void led_mode_breath(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_breath_step();
}

void led_mode_nhjb(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_nhjb_step();
}

void led_mode_liuguang(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_liuguang_step();
}

void led_mode_sbzl(void)
{
    uint8_t n = led_speed_steps();
    while(n--)
        led_mode_sbzl_step();
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

/******************************************************************************
* 函数名称: led_mode_star
* 功能描述: 繁星点点：随机位置出现星点并逐渐熄灭
* 说    明: 新星按 star_spawn_tbl 间隔出现（不补帧）；
*           衰减每 (速度档/2+1) 个基准节拍一级，与原分频一致
******************************************************************************/
void led_mode_star(void)
{
    uint8_t n;
    uint8_t lev = led_speed_index();

    if(led_timer_take(&led_timer_main, star_spawn_tbl[lev] * iLED_BASE_TICK, 1))
        led_mode_star_spawn();

    n = led_timer_take(&led_timer_aux, ((lev >> 1) + 1) * iLED_BASE_TICK, iLED_CATCHUP_MAX);
    while(n--)
        led_mode_star_decay();
}
