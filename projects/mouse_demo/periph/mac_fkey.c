/*******************************************************************************
* 文件名称: mac_fkey.c
* 文件描述: Mac 布局 F5 / F6 功能键（F5 听写，F6 勿扰模式）
*
* 说    明: LIB 版工程不含本文件，用编译好的 mac_fkey.o 代替（关闭 LTO、
*           不带调试信息，里面只有机器码），生成方法见 docs/LIB版生成说明.md
*           和 tools/make_lib.py。mac_fkey.h 里只有接口，实现细节只写在本文件里。
*
* 实现说明: 和苹果键盘一致，只在 Mac 布局下生效（Mac 正常表 C17 R4 = F5，C3 R2 = F6）。
*   F5 听写：Consumer 页 0x00CF（Voice Command）。放进多媒体值 multi_data_tmp_l / h，
*            之后和其他多媒体键一样发送，USB、蓝牙、2.4G 都走原多媒体通道。
*   F6 勿扰：Generic Desktop 页 0x9B（System Do Not Disturb）。macOS 上勿扰只能用这个
*            用法发（Consumer 页没有对应用法，见苹果《Accessory Design Guidelines》
*            表 14-3、14-4），所以单独走系统控制通道：
*            USB ：系统控制报告 [iREPORT_ID_SYS_KEY(3), 用法值低字节, 0]，usb_user_process.c 组帧
*            蓝牙：系统控制报告 ID 5，通知 [用法值低字节, 0]，mouse_hw.c 发送
*            2.4G：系统键包 [iRF_K_TYPE_KB, iRF_K_OTHER_SYS, 用法值]，状态变化时连发 8 包，
*                  按住期间每 64 个发送周期补发 2 包；接收器要把用法值放进它自己的
*                  系统控制报告转发（见 docs/2.4G接收器_勿扰键对接说明.md）
*   按住与松开：按键库 key_decode() 只在按住期间（bdefine_key_press）调用特殊键回调，
*            每次扫描一次，所以回调里每次都置值；松开后不再回调，扫描开头清零，
*            即发送释放。F5 这样处理和多媒体键表项的时序完全相同。
*******************************************************************************/
#include <stdint.h>
#include "om_driver.h"
#include "main.h"
#include "main_24g.h"
#include "matrix_key.h"
#include "key_code_define.h"
#include "mac_fkey.h"

//-----------------------------------------------------------------------------;
// 用法值和发送参数（只在本文件里出现）
//
#define FKEY_DICTATION_H        0x00        // Consumer 0x00CF Voice Command：macOS 听写
#define FKEY_DICTATION_L        0xCF
#define FKEY_DO_NOT_DISTURB     0x9B        // Generic Desktop 0x9B System Do Not Disturb：macOS 勿扰
#define FKEY_RF_REPEAT          8           // 2.4G：状态变化时连发包数（同多媒体键）
#define FKEY_RF_KEEPALIVE       2           // 2.4G：按住期间每次补发包数

//-----------------------------------------------------------------------------;
// 变量
//
static uint8_t                  fkey_scan;          // 本次扫描 F6 的用法值（0 = 未按）
static uint8_t                  fkey_sys;           // USB / 蓝牙：已安排发送的用法值
static uint8_t                  fkey_rf;            // 2.4G：当前用法值
static uint8_t                  fkey_rf_count;      // 2.4G：剩余发送包数

/******************************************************************************
* 函数名称: mac_fkey_hold()
* 功能描述: F5 / F6 按住处理
* 输入参数: key: 键码低 4 位
* 说    明: 由 key_decode_cmd_0x40() 在按住期间每次扫描调用一次
******************************************************************************/
void mac_fkey_hold(uint8_t key)
{
    switch(key)
    {
        case (iKEY_MAC_F5&0x0F):                    // 听写：按住期间保持，松开后扫描开头清零即释放
            multi_data_tmp_l = FKEY_DICTATION_L;
            multi_data_tmp_h = FKEY_DICTATION_H;
            break;
        case (iKEY_MAC_F6&0x0F):                    // 勿扰
            fkey_scan = FKEY_DO_NOT_DISTURB;
            break;
        default:
            break;
    }
}

/******************************************************************************
* 函数名称: mac_fkey_scan_clear()
* 功能描述: 清本次扫描的按键值
* 说    明: 每次扫描开头、全键盘锁定时调用；扫描函数在 RAM 里运行，本函数也放 RAM
******************************************************************************/
__RAM_CODE void mac_fkey_scan_clear(void)
{
    fkey_scan = 0;
}

/******************************************************************************
* 函数名称: mac_fkey_update()
* 功能描述: 按当前连接模式安排发送
* 说    明: key_process() 末尾调用（标准键有变化时 key_process() 提前返回，不会调用）
*           USB / 蓝牙：值变化且上一个报告已发出时，更新并置 bsys_data_send
*           2.4G：值变化时更新，连发 FKEY_RF_REPEAT 包
******************************************************************************/
void mac_fkey_update(void)
{
    if(iDEVICE_TYPE_24G == device_type)
    {
        if(fkey_scan != fkey_rf)
        {
            fkey_rf = fkey_scan;
            fkey_rf_count = FKEY_RF_REPEAT;
        }
    }
    else if(fkey_scan != fkey_sys)
    {
        if(0 == (m_data_flag & bsys_data_send))
        {
            fkey_sys = fkey_scan;
            m_data_flag |= bsys_data_send;
        }
    }
}

/******************************************************************************
* 函数名称: mac_fkey_reset()
* 功能描述: 切换模式时清除 USB / 蓝牙待发数据
******************************************************************************/
void mac_fkey_reset(void)
{
    fkey_sys = 0;
    m_data_flag &= ~bsys_data_send;
}

/******************************************************************************
* 函数名称: mac_fkey_sys_value()
* 功能描述: USB / 蓝牙系统控制报告的用法值（低字节）
* 说    明: 蓝牙发送函数在 RAM 里运行，本函数也放 RAM
******************************************************************************/
__RAM_CODE uint8_t mac_fkey_sys_value(void)
{
    return fkey_sys;
}

/******************************************************************************
* 函数名称: mac_fkey_rf_pending()
* 功能描述: 2.4G 是否还有系统键包要发
******************************************************************************/
__RAM_CODE uint8_t mac_fkey_rf_pending(void)
{
    return fkey_rf_count;
}

/******************************************************************************
* 函数名称: mac_fkey_rf_fill()
* 功能描述: 2.4G 填一包系统键包，返回包长度
******************************************************************************/
__RAM_CODE uint8_t mac_fkey_rf_fill(uint8_t *buf)
{
    if(fkey_rf_count)
        fkey_rf_count--;

    buf[0] = iRF_K_TYPE_KB;
    buf[1] = iRF_K_OTHER_SYS;
    buf[2] = fkey_rf;
    return iRF_K_LENGTH_OTHER;
}

/******************************************************************************
* 函数名称: mac_fkey_rf_keepalive()
* 功能描述: 2.4G 按住期间补发
******************************************************************************/
__RAM_CODE void mac_fkey_rf_keepalive(void)
{
    if(fkey_rf)
        fkey_rf_count = FKEY_RF_KEEPALIVE;
}

/******************************************************************************
* 函数名称: mac_fkey_rf_cancel()
* 功能描述: 2.4G 清除待发包（未连接接收器时）
******************************************************************************/
__RAM_CODE void mac_fkey_rf_cancel(void)
{
    fkey_rf_count = 0;
}
