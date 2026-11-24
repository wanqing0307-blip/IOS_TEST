/*******************************************************************************
* Copyright (c) 2012, SHENZHEN Co.,LTD.
* All rights reserved.
*
* 文件名称: sleep.c
* 功能说明:
* 修改说明: 无
*******************************************************************************/
#include "main_24g.h"

//-----------------------------------------------------------------------------;
// 常量定义区                                   // 定义只有函数内部使用的常量
//

//-----------------------------------------------------------------------------;
// 变量定义区
//
extern bool                 bperiph_int;
extern bool                 bdevice_led_status;

//----------------------------------------------------------------------------;
// 表格定义区

/******************************************************************************
* 函数名称: sleep_process
* 功能描述:
* 输入参数:
* 输出参数:
* 说    明: 无
******************************************************************************/

void sleep_process(void)
{
    uint8_t i;
    uint32_t rtc_value;
    static uint8_t no_dongle_time_count;

    switch(work_mode)
    {
        case iMCU_MODE_NORMAL:
            if(iRF_MODE_NORMAL == rf_mode_type)
            {                               // RF正常模式,检测是否进入休眠
                mRF_CE_LOW_IDLE;
                delayus(250);
                mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL1);
                drv_pmu_clear_wakeup();
                mMCU_MODE_SLEEP;

                sys_ctrl |= bscan_flag;
                bperiph_int = 0;
                device_led_off();
                logo_off();

                while(1)
                {
                    drv_pmu_set_low_power_mode(PMU_LPM_STOP1);
                    drv_wdt_keep_alive(OM_WDT);
                    if(bperiph_int)
                        break;

                    if((0 != USB_5V_PIN) && (0 == CHR_OK_PIN))
                    {
                        hint_led_status = bled_low;
                        led_status_hint();
                    }
                    else
                        goto MCU_MODE_PD_PRPCESS;
                }

                rf_ctrl_reg |= (brf_sync_time|brf_sync_status);
                mMCU_MODE_NORMAL;
                mouse_24g_init();
            }
            else if(iRF_MODE_SYNC == rf_mode_type)
            {
                if((rf_ctrl_reg & brf_sync_end)
                    &&(0==(rf_ctrl_reg & brf_sync_result)))
                {
                    mMCU_MODE_NO_DONGLE;
                    no_dongle_time_count = 0;
                    b24g_nromal = false;
                    kb_led_status = 0;
                    ble_24g_ctrl |= bom6239_sync_ing;
                }
            }

            break;
        case iMCU_MODE_NO_DONGLE:
            if(rf_ctrl_reg & brf_sync_end)
            {
                if(rf_ctrl_reg & brf_sync_result)
                {
                    rf_ctrl_reg |= brf_sync_time;
                    rf_mode_convert(iRF_MODE_SYNC);
                }
                else
                {
                    if(bkey_24g_press == (device_key&bkey_24g_press))
                        return;

                    rf_ctrl_reg &= ~(brf_sync_time|brf_sync_status);

                    mouse_periph_wakeup_enable(WAKEUP_ALL_DISABLE);
                    clr_rgb();              // 只清灯效；保留 LEDC 输出第 1 颗指示灯，避免重试时整条灯瞬间熄灭
                    key_tx_count = 0;
                    mmk_tx_count = 0;
                    sys_tx_count = 0;
                    rtc_value = drv_rtc_read(OM_RTC);
                    do
                    {
                        // 提示灯闪烁期间不进 STOP1：STOP1 会暂停系统时钟，打断 LEDC 正在发送的帧，
                        // 表现为闪灯时短暂发暗；提示结束后 ARGB 已关闭，再恢复 STOP1 省电
                        if(iDEFAULT_NO_DONGLE_MODE_TIME > no_dongle_time_count)
                            delayus(500);
                        else
                            rf_normal_mode_sleep();
                        drv_wdt_keep_alive(OM_WDT);

                        i++;
                        if(3==(i&0x07))
                        {
                            led_hint_process();
                        }
                        else if(0==(i&0x07))
                        {
                            scan_mode_swtich(iMODE_CHECK_NO_DELAY);
                            if(true == bmode_swtich_valid)
                                return;
                            logo_process();
                        }
                        matrixkey_scan();

                        if(iDEFAULT_NO_DONGLE_MODE_TIME > no_dongle_time_count)
                        {
                            device_led_process();           // no-dongle 阶段由主循环统一驱动提示灯

                            if(false == bargb_valid)        // 进入 no-dongle 前 logo_off() 已关闭 ARGB
                            {
                                OM_GPIO0->DATAOUTCLR = (1<<ARGB_OUT_PIN);
                                OM_GPIO0->DATAOUTSET = (1<<ARGB_OFF_PIN);
                                ledc_init();
                            }
                            led_status_hint();
                            drv_ledc_write();               // 无 RF 数据发送，这里主动刷新第 1 颗指示灯
                            if(mMS_TO_TICK(850) < (drv_rtc_read(OM_RTC)-rtc_value))
                                break;
                        }
                        else
                        {
                            if((0!=key_tx_count)||(0!=mmk_tx_count)||(0!=sys_tx_count))
                                break;

                            hint_led_status &= ~bled_hint_mode;
	                        led_status_hint();
                            if((0 == USB_5V_PIN) || (0 != CHR_OK_PIN))
                                goto MCU_MODE_PD_PRPCESS;
                        }
                    }
                    while(1);

                    if((0!=key_tx_count)||(0!=mmk_tx_count)||(0!=sys_tx_count))
                    {
                        no_dongle_time_count=0;
                        rf_ctrl_reg |= (brf_sync_time|brf_sync_status);
                    }

                    no_dongle_time_count++;
                    if(iDEFAULT_NO_DONGLE_MODE_TIME < no_dongle_time_count)
                    {
                        if(0 == (bat_status&bcharging))
                            goto MCU_MODE_PD_PRPCESS;
                    }

                    mouse_24g_init();
                    sys_ctrl |= bscan_flag;

                }
            }
            break;
        default:                                // iMCU_MODE_PD:
MCU_MODE_PD_PRPCESS:
            logo_off();
            mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL2);
            drv_pmu_set_low_power_mode(PMU_LPM_DEEP_SLEEP);
            while(1);
            break;
    }
}
