/*******************************************************************************
* 文件名称: mac_fkey.h
* 文件描述: Mac 布局 F5 / F6 功能键模块接口（F5 听写，F6 勿扰模式）
*
* 说    明: 内部版工程带实现源码 mac_fkey.c；对外版工程只有本头文件和预编译的
*           mac_fkey.o（机器码，不含源码和调试信息），按目标文件链接，
*           用法和 userdefined_key.o 相同。
*           本文件内部版、对外版相同，只写接口，不写实现细节。
*******************************************************************************/
#ifndef _MAC_FKEY_H_
#define _MAC_FKEY_H_

#include <stdint.h>

#define MAC_FKEY_VERSION        0x0100      // 模块版本 V1.00；接口有变化时更新，并重新生成对外版

//-----------------------------------------------------------------------------;
// 按键扫描
//
void    mac_fkey_hold(uint8_t key);         // Mac 表 F5 / F6 特殊键回调里调用（只在按住期间、每次扫描调用一次），
                                            // key = 键码低 4 位（iKEY_MAC_F5 / iKEY_MAC_F6 & 0x0F）
void    mac_fkey_scan_clear(void);          // 每次扫描开头、全键盘锁定时调用：清本次扫描的按键值
void    mac_fkey_update(void);              // key_process() 末尾调用：按当前连接模式安排发送
void    mac_fkey_reset(void);               // mode_swtich_init() 调用：切换模式时清除待发数据

//-----------------------------------------------------------------------------;
// USB / 蓝牙：系统控制报告（m_data_flag 的 bsys_data_send 置位时发送）
//
uint8_t mac_fkey_sys_value(void);           // 本次要发的 16 位用法值的低字节（高字节为 0）

//-----------------------------------------------------------------------------;
// 2.4G
//
uint8_t mac_fkey_rf_pending(void);          // 还有包要发时返回非 0
uint8_t mac_fkey_rf_fill(uint8_t *buf);     // 填一包到 buf（至少 3 字节），返回包长度
void    mac_fkey_rf_keepalive(void);        // 按住期间补发：每 64 个发送周期调用一次
void    mac_fkey_rf_cancel(void);           // 清除待发包（未连接接收器时）

#endif
