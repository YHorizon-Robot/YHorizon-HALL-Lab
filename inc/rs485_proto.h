/*!
    \file    rs485_proto.h
    \brief   霍尔设备的 RS485 帧协议

    物理层：USART0，PA9 发、PA10 收，8N1。PB5 高电平发送，低电平接收
    （SIT3088 的 DE 与 RE# 接在一起）。默认 115200，设备号 1。

    帧格式，多字节小端：

        0  SOF0  0xA5
        1  SOF1  0x5A
        2  DST   目标设备号，0xFF 广播
        3  SRC   源设备号，主机用 0x00
        4  TYPE  设备类型，0xFF 表示不限
        5  CMD   命令，应答时最高位置 1
        6  SEQ   序号，应答原样带回
        7  LEN   载荷长度
        8  DATA  LEN 字节
        .  CRC16 Modbus，覆盖 DST 到载荷末尾

    一条总线上设备号不能重复，编码器和以后的位置传感器也共用这套号。
    点名时写上具体类型，避免两台不同类型设备撞号时一起回答。
    广播只执行设置，不回答。

    设备类型：
        0x01  角度编码器（本固件）
        0x02  预留给霍尔位置传感器
        0xFF  任意类型

    命令（主机 -> 设备）：
        0x01  读信息。载荷空。
              应答：类型、设备号、模式、波特率序号、上报周期、固件版本、
              校准有效、标志、机械零点。
        0x02  设置设备号。载荷 1 字节，1..254。应答带回新设备号。
        0x03  设置波特率。载荷 1 字节序号。先按旧波特率应答，再切换。
        0x04  设置上传模式。载荷：模式 1 字节，周期毫秒 uint16。
              0 主从，只在查询后回答；1 主动上报，同时仍回答查询。
        0x05  把当前参数写入 Flash。
        0x06  恢复出厂。设备号 1，115200，主从，20 ms，清零点和校准。
        0x10  读一帧角度。应答在状态字节之后是角度 int32（0.01°）、
              幅度 int16、三路原码 uint16、标志。
        0x21  设置机械零点。载荷空则把当前角度记为 0°；
              载荷 uint16 则直接写入要减掉的角度，范围 0..35999。
        0x22  清除机械零点。
        0x23  开始校准。之后转满至少一圈。
        0x24  结束校准。峰峰值不够时保持校准中，可以继续转再结束。
              成功应答：三路偏置 int16、三路增益 uint16（Q12，4096 = 1）。
        0x25  清除校准，回到三路平均值去零。
        0x26  读取校准。标志、三路偏置、三路增益。

    设备主动帧：
        0x30  角度上报，没有状态字节，载荷与 0x10 应答里状态字节之后相同。
              目标固定 0x00，源是本机设备号。

    应答命令 = 请求命令 | 0x80，载荷第一字节是状态：
        0x00 成功
        0x01 长度不对
        0x02 参数超出范围
        0x03 当前状态不允许
        0x04 校准数据不够
        0x05 Flash 写入失败
        0x06 不认识的命令

    写 Flash 的命令应答可能要几十毫秒。主机超时建议不小于 100 ms。
    主动上报的总线上同时只让一台设备处于上报模式。

    波特率序号：0=9600，1=19200，2=38400，3=57600，4=115200，
    5=230400，6=460800，7=921600。

    标志位与 HALL_FLAG_* 相同：bit0 磁场弱，bit1 校准有效，bit2 正在校准。
*/

#ifndef RS485_PROTO_H
#define RS485_PROTO_H

#include <stdint.h>

#define RS485_SOF0                  0xA5U
#define RS485_SOF1                  0x5AU
#define RS485_ADDR_HOST             0x00U
#define RS485_ADDR_BCAST            0xFFU

#define RS485_TYPE_HALL_ENCODER     0x01U
#define RS485_TYPE_HALL_POSITION    0x02U
#define RS485_TYPE_ANY              0xFFU

#define RS485_MODE_POLL             0U
#define RS485_MODE_REPORT           1U

#define RS485_CMD_GET_INFO          0x01U
#define RS485_CMD_SET_ID            0x02U
#define RS485_CMD_SET_BAUD          0x03U
#define RS485_CMD_SET_MODE          0x04U
#define RS485_CMD_SAVE              0x05U
#define RS485_CMD_FACTORY           0x06U
#define RS485_CMD_GET_DATA          0x10U
#define RS485_CMD_SET_ZERO          0x21U
#define RS485_CMD_CLEAR_ZERO        0x22U
#define RS485_CMD_CAL_BEGIN         0x23U
#define RS485_CMD_CAL_END           0x24U
#define RS485_CMD_CAL_CLEAR         0x25U
#define RS485_CMD_GET_CAL           0x26U
#define RS485_CMD_REPORT            0x30U
#define RS485_CMD_RESPONSE          0x80U

#define RS485_OK                    0x00U
#define RS485_ERR_LEN               0x01U
#define RS485_ERR_PARAM             0x02U
#define RS485_ERR_STATE             0x03U
#define RS485_ERR_CAL               0x04U
#define RS485_ERR_FLASH             0x05U
#define RS485_ERR_CMD               0x06U

#define RS485_BAUD_9600             0U
#define RS485_BAUD_19200            1U
#define RS485_BAUD_38400            2U
#define RS485_BAUD_57600            3U
#define RS485_BAUD_115200           4U
#define RS485_BAUD_230400           5U
#define RS485_BAUD_460800           6U
#define RS485_BAUD_921600           7U
#define RS485_BAUD_COUNT            8U

#define RS485_FW_VERSION            0x0100U

/*!
    \brief      从 Flash 恢复参数，打开 USART0，方向脚保持接收
    \param[in]  none
    \param[out] none
    \retval     none
*/
void rs485_proto_init(void);

/*!
    \brief      解析收到的帧，到点则主动上报
    \param[in]  none
    \param[out] none
    \retval     none
*/
void rs485_proto_poll(void);

/*!
    \brief      USART0 中断里调用
    \param[in]  none
    \param[out] none
    \retval     none
*/
void rs485_usart_isr(void);

/*!
    \brief      当前设备号
    \retval     1..254
*/
uint8_t rs485_proto_id(void);

/*!
    \brief      当前波特率
    \retval     bit/s
*/
uint32_t rs485_proto_baud(void);

/*!
    \brief      当前上传模式
    \retval     RS485_MODE_POLL 或 RS485_MODE_REPORT
*/
uint8_t rs485_proto_mode(void);

#endif /* RS485_PROTO_H */
