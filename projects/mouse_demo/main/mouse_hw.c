/* ----------------------------------------------------------------------------
 * Copyright (c) 2020-2030 OnMicro Limited. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *   3. Neither the name of OnMicroelectronics nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * -------------------------------------------------------------------------- */

/**
 * @file     mouse_sw.c
 * @brief    ble mouse sensor & wheel scan use io simulate by hardware
 * @date     20 JUN 2024
 * @author   OnMicro SW Team
 *
 * @version
 * Version 1.0
 *  - Initial release
 *
 * @{
 */

/*******************************************************************************
 * INCLUDES
 */
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "om_driver.h"
#include "ex2_app_mouse.h"
#include "mouse.h"
#include "ex2_api.h"
#include "ex2_hal.h"
#include "main.h"
#include "led.h"
#include "lvd.h"

#include "matrix_key.h"
#include "mac_fkey.h"
#include "flash_process.h"
#include "userdefined_key.h"
#include "time_delay.h"

#include "usb_cfg_info.h"

/*******************************************************************************
 * CONST & VARIABLES
 */
 ms_sleep_level_t ms_sleep_level = MS_SLEEP_LEVEL0;
//-----------------------------------------------------------------------------
// BT5.4 KB-1
//
const char                  bt_kb_name_tbl_bt0[MOUSE_NAME_MAX_LEN] ="WK50_104BT1";
const char                  bt_kb_name_tbl_bt1[MOUSE_NAME_MAX_LEN] ="WK50_104BT2";
const char                  bt_kb_name_tbl_bt2[MOUSE_NAME_MAX_LEN] ="WK50_104BT3";

char                        bt_kb_name[MOUSE_NAME_MAX_LEN];

uint8_t                     ble_interval;

bool                        bms_sleep_rtc;

extern bool                 bperiph_int;
extern uint32_t             adv_interval_ms;

bool                        bradio_receive;
/*********************************************************************
 * LOCAL FUNCTIONS
 */
pmu_lpm_t pm_checker_callback(void)
{
    if(ms_sleep_level < MS_SLEEP_LEVEL2) {
        bms_sleep_rtc = false;
        if(MS_SLEEP_LEVEL0 == ms_sleep_level)
        {
            if(true==bargb_valid)
                return PMU_LPM_IDLE;
            if(0 != OM_RTC->TICK)
                return PMU_LPM_IDLE;
        }

        /* 距离下次唤醒时间剩余300us， 不进入休眠, 否则进入休眠 */
        if ((ex2_stk_sleep_duration_get() > 10))
            return PMU_LPM_STOP1;           // PWM没有打开，可以进入STOP1
    }
    else
    {
        if((0 == USB_5V_PIN) || (0 != CHR_OK_PIN))
        {
            ex2_stk_uninit();
            //进入深睡眠前需要存一些配置信息(如dpi、模式、地址等)到HS_PMU->SCRATCH, 醒来后可通过OM_CPM->RSTCTRL判断复位原因决定广播还是回连。

            logo_off();
            mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL2);
            drv_pmu_set_low_power_mode(PMU_LPM_DEEP_SLEEP);
            while(1);        
        }
        else
        {
            if(false == bms_sleep_rtc)
            {
                bms_sleep_rtc = true;

                drv_rtc_control(OM_RTC, RTC_CONTROL_ALARM0_INT_EN, NULL);
                NVIC_EnableIRQ(RTC_IRQn);
                drv_rtc_set_alarm0(drv_rtc_read(OM_RTC) + mMS_TO_TICK(610));
                logo_off();

                mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL2);
            }
            device_led_off();
            hint_led_status &= ~bled_hint_mode;

            if(bperiph_int)
                NVIC_SystemReset();

            if((ex2_stk_sleep_duration_get() > 10))
            {
                if(0x00000000 == OM_PWM->PWMCON)
                    return PMU_LPM_STOP1;           // PWM没有打开，可以进入STOP1
                else
                    return PMU_LPM_IDLE;
            }
        }
    }

    return PMU_LPM_IDLE;
}

extern bool ex2_hid_tx_notification_mouse(uint8_t *buf);
extern bool ex2_hid_tx_notification_keyboard(uint8_t *buf);
extern bool ex2_hid_tx_notification_bitmap_kb(uint8_t *buf);
extern bool ex2_hid_tx_notification_consumer_control(uint8_t *buf);
extern bool ex2_hid_tx_notification_system_control(uint8_t *buf);
extern bool profile_bas_tx_notification(uint8_t level);
extern  uint8_t hid_keyboard_out_report_value;

/*********************************************************************
 * 诊断：主机最近一次更新后的连接参数，用 J-Link 在 Keil Watch 窗口查看
 * dbg_conn_interval_tick：连接间隔，RTC tick（约 32 tick = 1ms，15ms ≈ 490）
 * dbg_conn_latency：从机延迟；dbg_conn_update_cnt：连上后参数更新的次数（0 = 主机没有更新过参数）
 */
volatile uint32_t                   dbg_conn_interval_tick;
volatile uint32_t                   dbg_conn_timeout_tick;
volatile uint16_t                   dbg_conn_latency;
volatile uint16_t                   dbg_conn_update_cnt;

typedef struct {                                 // 与协议栈 ll_conn_param_t 相同（见 ex2_api.h）
    uint32_t tick_winsize;
    uint32_t tick_winoff;
    uint32_t tick_interval;
    uint32_t tick_timeout;
    uint16_t latency;
    uint16_t instance;
    uint8_t  chn_map[5];
} dbg_ll_conn_param_t;

/* 覆盖协议栈的弱函数：主机更新连接参数时调用 */
void ex2_ll_hci_connect_param_update_complete(uint8_t *buf)
{
    dbg_ll_conn_param_t param;
    memcpy(&param, buf, sizeof(param));        // Cortex-M0 不支持非对齐访问，先拷出来
    dbg_conn_interval_tick = param.tick_interval;
    dbg_conn_timeout_tick  = param.tick_timeout;
    dbg_conn_latency       = param.latency;
    dbg_conn_update_cnt++;
}

/*********************************************************************
 * 诊断（调试用）：浅休眠时协议栈的状态，按 FN+P 以文字输出
 * 连接上下文在协议栈库内部不可访问，这里只用库公开的接口：
 *   比较“连接事件计数器的增量”和“应用实际被唤醒的次数”，
 *   两者接近 = 每个连接事件都醒（latency 没生效）；事件数远大于唤醒数 = latency 在跳。
 */
extern bool stk_is_wait_sending(void);
extern uint32_t ex2_ll_get_conn_super_timeout(void);

static uint32_t dbg_l1_rtc0, dbg_l1_rtc;
static uint16_t dbg_l1_evt0, dbg_l1_evt;
static uint16_t dbg_l1_wakes, dbg_l1_enter, dbg_bounce, dbg_l1_waitcnt;
static uint8_t  dbg_bounce_flag;

static void dbg_snapshot(uint8_t entry)         // entry = 1：刚进入浅休眠，记起点
{
    uint32_t now = drv_rtc_read(OM_RTC);
    uint16_t evt = ex2_ll_get_conn_event_counter();
    if(entry)
    {
        dbg_l1_rtc0 = now;
        dbg_l1_evt0 = evt;
        dbg_l1_wakes = 0;
        dbg_l1_waitcnt = 0;
    }
    else
        dbg_l1_wakes++;
    dbg_l1_rtc = now;
    dbg_l1_evt = evt;
    if(stk_is_wait_sending())
        dbg_l1_waitcnt++;
}

static void dbg_put_char(char ch)
{
    uint8_t c = 0;
    if((ch >= 'A') && (ch <= 'Z'))       c = 0x04 + (ch - 'A');
    else if(ch == '0')                   c = 0x27;
    else if((ch >= '1') && (ch <= '9'))  c = 0x1E + (ch - '1');
    else if(ch == ' ')                   c = 0x2C;
    else if(ch == '\n')                  c = 0x28;
    if(c && (dbg_seq_len < DBG_SEQ_MAX))
        dbg_seq[dbg_seq_len++] = c;
}

static void dbg_item(char tag, uint32_t v)
{
    char b[10];
    uint8_t n = 0;
    dbg_put_char(tag);
    do { b[n++] = (char)('0' + (v % 10)); v /= 10; } while(v && (n < 10));
    while(n) dbg_put_char(b[--n]);
    dbg_put_char(' ');
}

/* 生成一行诊断文字（本次浅休眠以来）：
 * I 连接间隔(tick, 32768/s)  T 超时(tick)  C 连接事件数  N 应用唤醒次数  P 平均唤醒周期(tick)
 * W 唤醒时还有数据待发的次数  E 进入浅休眠次数  B 被拉回 LEVEL0 次数  X 拉回时的数据标志
 * U 参数更新次数  V 更新后的延迟 */
void dbg_info_build(void)
{
    uint32_t dt = dbg_l1_rtc - dbg_l1_rtc0;
    dbg_seq_len = 0;
    dbg_item('I', ex2_ll_get_conn_interval_tick());
    dbg_item('T', ex2_ll_get_conn_super_timeout());
    dbg_item('C', (uint16_t)(dbg_l1_evt - dbg_l1_evt0));
    dbg_item('N', dbg_l1_wakes);
    dbg_item('P', dbg_l1_wakes ? (dt / dbg_l1_wakes) : 0);
    dbg_item('W', dbg_l1_waitcnt);
    dbg_item('E', dbg_l1_enter);
    dbg_item('B', dbg_bounce);
    dbg_item('X', dbg_bounce_flag);
    dbg_item('U', dbg_conn_update_cnt);
    dbg_item('V', dbg_conn_latency);
    dbg_put_char('\n');
}

/*********************************************************************
 * LOCAL FUNCTIONS
 */
__RAM_CODE uint8_t ex2_app_mouse_scan(void)
{
    uint8_t temp_buff[8];

    if (!hid_notification_is_enabled()) {
        return 0;
    }

    ble_24g_ctrl |= bom6239_sync_ok;

    if(MS_SLEEP_LEVEL0 == ms_sleep_level)
    {
        matrixkey_scan();
        logo_process();
        drv_ledc_write();

        lvd_scan_count++;
        if(0==(lvd_scan_count&0x01))
            device_led_process();               // 15ms
        else
            led_hint_process();
    }
    else
    {
        logo_off();
        led_hint_process();
    }
	scan_mode_swtich(iMODE_CHECK_DELAY);

    kb_led_status = hid_keyboard_out_report_value;
    if(0!=(m_data_flag&bstd_kb_data_send))
    {
        temp_buff[0] = std_key[0];
        temp_buff[1] = 0;
        memcpy(temp_buff+2, std_key+1, 6);
        if(true == ex2_hid_tx_notification_keyboard(temp_buff))
        {
            rtc_init_value = drv_rtc_read(OM_RTC);  // 按键有数据，重置休眠计时
            m_data_flag &= ~bstd_kb_data_send;
            key_tx_count = 0;
        }
        return 1;
    }
    else if(0!=(m_data_flag&bmmk_data_send))
    {
        temp_buff[0] = mmk_data_l;
        temp_buff[1] = mmk_data_h;
        if(true == ex2_hid_tx_notification_consumer_control(temp_buff))
        {
            rtc_init_value = drv_rtc_read(OM_RTC);  // 多媒体键有数据，重置休眠计时
            m_data_flag &= ~bmmk_data_send;
            mmk_tx_count = 0;
        }
        return 1;
    }
    else if(0!=(m_data_flag&bsys_data_send))
    {                                           // 系统控制报告
        temp_buff[0] = mac_fkey_sys_value();    // 16 位用法值，低字节
        temp_buff[1] = 0;                       // 高字节
        if(true == ex2_hid_tx_notification_system_control(temp_buff))
        {
            rtc_init_value = drv_rtc_read(OM_RTC);  // 有数据，重置休眠计时
            m_data_flag &= ~bsys_data_send;
        }
        return 1;
    }
    else if(0!=(m_data_flag&bstd_ex_data_send))
    {
        if(true == ex2_hid_tx_notification_bitmap_kb(std_key+7))
        {
            rtc_init_value = drv_rtc_read(OM_RTC);  // 扩展按键有数据，重置休眠计时
            m_data_flag &= ~bstd_ex_data_send;
            key_ex_tx_count = 0;
        }
        return 1;
    }
    else if(false != bbat_update)
    {
        if(true == profile_bas_tx_notification(bat_value))
            bbat_update=false;
        return 1;
    }
    else if(0!=m_tx_count)
    {                                           /* 未扫描到数据 */
        temp_buff[0] = mouse_data_key&0x1F;       // button
        temp_buff[1] = ((mouse_x>>0)&0xFF);       // x
        temp_buff[2] = (((mouse_x>>8)&0x0F)|((mouse_y<<4)&0xF0)); // y
        temp_buff[3] = ((mouse_y>>4)&0xFF);       //
        temp_buff[4] = mouse_data_z;              // wheel
        temp_buff[5] = 0;                         //til
        if(true == ex2_hid_tx_notification_mouse(temp_buff))
        {
            rtc_init_value = drv_rtc_read(OM_RTC);  // 鼠标有数据，重置休眠计时
            m_data_flag &=~(bmouse_data_send);
            mouse_x = 0;
            mouse_y = 0;
            mouse_data_z = 0;
            m_tx_count = 0;
            return 1;
        }
    }
    else
    {
        if(true == bsave_ble_id)
        {
            bsave_ble_id = false;
            flash_save_ble_addr();
        }
    }
    return 0;
}

extern ex2_gap_ppcp_t       ex2_gap_ppcp_value;
extern mouse_adv_state_t    mouse_state;
extern bool                 bscan_sensor_en;

extern uint16_t ll_ctrl_get_peer_company_id(void);
extern void stk_notify_event_app(uint32_t evt);

/*********************************************************************
 * LOCAL FUNCTIONS
 */
void app_ble_evt_dispatch(uint32_t events)
{
    static uint32_t connect_start_time = 0; /* work around intel bluetooth bug */
    if (events & EX2_EVENT_APP_CONNECT_IND) {
        mouse_periph_wakeup_enable(WAKEUP_ALL_DISABLE);
        drv_rf_set_tx_power(DRV_RF_PA_PWR_0dBm);
        /* stop polling the peripheral*/
        ex2_ll_polling_time_stop();
        connect_start_time = 0;
    }
    if (events & (EX2_EVENT_APP_SLAVE_TERMINATED)) {
        uint8_t bda[6];
        drv_ble_get_bda(bda);
        /* reset rtc & rf module */
        ex2_hal_uninit();
        /* Avoid conn activites during ms delay. */
        __disable_irq();
        ex2_stk_init(bda);
        mouse_state = MOUSE_LE_STATE_DISCONN;
        ex2_app_mouse_broadcast_fsm();

        __enable_irq();
    }
    if (events & EX2_EVENT_APP_CONN_TIMEOUT) {
        __disable_irq();
        NVIC_SystemReset();
    }

    if(events & EX2_EVENT_APP_MASTER_TERMINATED) {
        ms_sleep_level = MS_SLEEP_LEVEL2;
    }
    if (events & (EX2_EVENT_APP_PAIR_COMPLETED | EX2_EVENT_APP_RECONNECT)) {
        if (events & EX2_EVENT_APP_RECONNECT) {
            hid_notification_enable(1);
        }
    }
    if (events & EX2_EVENT_APP_VERSION_EXCHANGE) {
        uint16_t vresion_info = ll_ctrl_get_peer_company_id();

        bradio_receive = false;
        if(vresion_info == 0x0002/* Intel */)  /* work around intel bluetooth bug */
            connect_start_time = drv_rtc_read(OM_RTC);
        else if(vresion_info == 0x01BF)
            bradio_receive = true;
    }
    if (events & EX2_EVENT_APP_CONN_SLAVE) {
        if (!(OM_RTC->INTMASK & RTC_INTMASK_ALARM0_INTRAW_STATUS_MASK)) {
            if (hid_notification_is_enabled())
            {
                ex2_l2cap_update_conn_para(&ex2_gap_ppcp_value);
                ex2_ll_polling_time_start();
                bbat_update = true;

                if(ble_24g_ctrl&bom6239_conning)
                {
                    ble_24g_ctrl |= bom6239_conn_ok;
                    if(false == bradio_receive)
                        bsave_ble_id = true;
                }
            }
        }
        if(connect_start_time) { /* work around intel bluetooth bug */
            if (EX2_TIME_OLDER_THAN(connect_start_time+EX2_MS_TO_TICK(10000), drv_rtc_read(OM_RTC))) {
                if(!hid_notification_is_enabled()) {
                    connect_start_time = 0;
                    stk_notify_event_app(EX2_EVENT_APP_CONN_TIMEOUT);
                }
            }
        }
    }
    if (events & EX2_EVENT_APP_ADV_TIMEOUT) {
        if(mouse_state == MOUSE_LE_STATE_STOP) { /* 广播一直没有master连接，超时进入deepsleep*/
            ms_sleep_level = MS_SLEEP_LEVEL2;
        } else {
            ex2_app_mouse_broadcast_fsm();
        }
    }

    if(events & EX2_EVENT_APP_ADV_EVENT_DONE) { /* 一个广播事件结束 */
        sys_tick_count += adv_interval_ms;
        ble_24g_ctrl |= bom6239_sync_ing;
        matrixkey_scan();
        scan_mode_swtich(iMODE_CHECK_DELAY);
        led_hint_process();
        logo_process();                     // sync_ing 时只清灯效，保留第1颗模式指示灯
        drv_ledc_write();                   // 未连接时也要输出配对/回连提示
        device_led_process();
    }
}

/*********************************************************************
 * LOCAL FUNCTIONS
 */

__RAM_CODE void drv_rtc_isr_callback(void)
{
    uint32_t rtc_count;

    rtc_count = drv_rtc_read(OM_RTC);
    if(!rtc_init_value)
    {
        rtc_init_value = rtc_count;
    }

    if(MS_SLEEP_LEVEL2 == ms_sleep_level)
    {
        while (0 == (OM_RTC->CR & RTC_CR_ALARM0_WRITE_STATUS_MASK));
        OM_RTC->ALARM0 = (rtc_count + mMS_TO_TICK(256));
        drv_wdt_keep_alive(OM_WDT);
        led_hint_process();
        led_status_hint();
        return;
    }

    if (hid_notification_is_enabled())
    {
        switch (ms_sleep_level)
        {
            case MS_SLEEP_LEVEL0:
                if(ble_sleep_1_time_tbl[sleep_time_ble&0x07] > (rtc_count-rtc_init_value))
                {
                    ex2_ll_set_next_polling_time(0);
                }
                else
                {
                    rtc_init_value = rtc_count;
                    ms_sleep_level = MS_SLEEP_LEVEL1;
                    mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL1);

                    logo_off();
                    dbg_l1_enter++;
                    dbg_snapshot(1);
                    ex2_gap_enable_latency();   // 主机的 ATT 读写会让协议栈暂停 latency 约 2s，浅休眠时恢复
                    ex2_ll_set_next_polling_time(1);
                }
                sys_tick_count += 8;

                break;
            case MS_SLEEP_LEVEL1:
                if((ble_sleep_2_time_tbl[(sleep_time_ble>>4)&0x07] > (rtc_count-rtc_init_value))
                    ||((0 != (bat_status&bcharging))&&(0==(bat_status&bcharge_ok))))
                {
                    dbg_snapshot(0);
                    ex2_gap_enable_latency();   // 同上：主机每次 ATT 读写后都重新允许 latency，否则每个连接事件都要醒
                    ex2_ll_set_next_polling_time(1);
                }
                else
                {
                    rtc_init_value = rtc_count;
                    ex2_ll_set_next_polling_time(0);
                    ms_sleep_level = MS_SLEEP_LEVEL2;
                }
                sys_tick_count += 300;

                break;
            default:
                break;
        }
    }

    if(ex2_app_mouse_scan())
    {
        if(MS_SLEEP_LEVEL0 != ms_sleep_level)
        {
            dbg_bounce++;                       // 诊断：浅休眠里有数据要发，被拉回 LEVEL0
            dbg_bounce_flag = m_data_flag | (bbat_update ? 0x80 : 0);
            ms_sleep_level = MS_SLEEP_LEVEL0;
            mouse_periph_wakeup_enable(WAKEUP_SLEEP_LEVEL0);
        }

        ex2_stk_interrupt();
    }
}
