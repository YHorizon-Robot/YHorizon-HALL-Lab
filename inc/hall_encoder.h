/*!
    \file    hall_encoder.h
    \brief   三路 120° 线性霍尔径向磁铁角度解算
*/

#ifndef HALL_ENCODER_H
#define HALL_ENCODER_H

#include <stdint.h>

/*!
    机械安装约定，从上往下看、逆时针为正：
      - U1 / PA0 / ADC_IN0 放在 0°
      - U2 / PA1 / ADC_IN1 放在 120°
      - U4 / PA4 / ADC_IN4 放在 240°
    径向充磁磁铁的转角为 θ 时，三路输出交流分量是 cos(θ)、cos(θ-120°)、cos(θ-240°)。
    相位顺序和 PCB 不一致时，只改下面三个下标。转动方向反了就把 HALL_ANGLE_SIGN 改成 -1。
*/
#define HALL_PHASE_0_DEG            0
#define HALL_PHASE_120_DEG          1
#define HALL_PHASE_240_DEG          2
#define HALL_ANGLE_SIGN             1

/*! 低于该峰值（ADC 计数）认为磁场太弱，角度只作参考 */
#define HALL_AMP_WEAK               80

/*! 校准结束时，任一路峰峰值低于该计数就认为没有转满一圈 */
#define HALL_CAL_SPAN_MIN           400

/*! Q12 增益，4096 表示 1.0 倍 */
#define HALL_CAL_GAIN_ONE           4096U

#define HALL_CAL_OK                 0U
#define HALL_CAL_IDLE               1U
#define HALL_CAL_WEAK               2U

#define HALL_FLAG_WEAK              0x01U
#define HALL_FLAG_CAL_VALID         0x02U
#define HALL_FLAG_CAL_RUN           0x04U
#define HALL_FLAG_OUT               0x08U
/*! 已确认有磁铁，angle_cdeg 是真实角度或位置 */
#define HALL_FLAG_MAGNET            0x10U
/*! 有一路贴近 ADC 的 0 或满量程，磁铁太近，读数不可信 */
#define HALL_FLAG_SAT               0x20U

/*! 没有确认磁铁时 angle_cdeg 固定为这个值，不会和任何真实读数混淆 */
#define HALL_NO_MAGNET              (-2147483647 - 1)

/*!
    判断有磁铁的条件：
      - 三路都不贴近 0 或 4095；
      - 幅度先连续 HALL_MAG_ON_MS 不低于进入门限，才认为有磁铁；
      - 之后只要连续 HALL_MAG_OFF_MS 低于退出门限，立刻认为没有。
    进入慢、退出快，宁可短暂不出数，也不在没有磁铁时给出角度或位置。
*/
#define HALL_MAG_ON_MS              100U
#define HALL_MAG_OFF_MS             5U
#define HALL_RAW_SAT_LO             32U
#define HALL_RAW_SAT_HI             4063U
/*! 角度模式门限，单位 ADC 计数。未校准时三路偏置差会造成约 110 的假幅度 */
#define HALL_ENC_MAG_ON             400
#define HALL_ENC_MAG_OFF            250

/*!
    \brief      一次采样结果
*/
typedef struct {
    uint16_t raw[3];       /*!< U1、U2、U4 的 12 位 ADC 原码 */
    int32_t angle_cdeg;    /*!< 0..35999，单位 0.01°，已减去机械零点；没有磁铁时为 HALL_NO_MAGNET */
    int32_t amplitude;     /*!< 单路交流峰值，单位 ADC 计数 */
} hall_sample_t;

/*!
    \brief      有无磁铁的去抖状态
*/
typedef struct {
    uint32_t since_ms;     /*!< 当前这种不一致状态开始的时刻 */
    uint8_t timing;        /*!< since_ms 是否有效 */
    uint8_t present;       /*!< 1 表示已确认有磁铁 */
} hall_presence_t;

/*!
    \brief      按进入、保持条件更新有无磁铁
    \param[in]  p: 去抖状态
    \param[in]  enter: 本次满足进入门限
    \param[in]  keep: 本次满足保持门限
    \retval     1 有磁铁，0 没有
*/
uint8_t hall_presence_update(hall_presence_t *p, uint8_t enter, uint8_t keep);

/*!
    \brief      回到“没有磁铁”，重新计时
    \param[in]  p: 去抖状态
    \retval     none
*/
void hall_presence_reset(hall_presence_t *p);

/*!
    \brief      三路中有没有贴近 0 或满量程的
    \param[in]  raw: 三路原码
    \retval     1 饱和，0 正常
*/
uint8_t hall_raw_saturated(const uint16_t raw[3]);

/*!
    \brief      三路偏置和相对增益
    偏置是一圈里的 (min+max)/2。增益以 U1 的峰峰值为基准，Q12。
*/
typedef struct {
    int16_t offset[3];
    uint16_t gain_q12[3];
    uint8_t valid;
} hall_cal_t;

/*!
    \brief      配置 PA0/PA1/PA4 为模拟输入，并启动扫描 + DMA
    \param[in]  none
    \param[out] none
    \retval     none
*/
void hall_encoder_init(void);

/*!
    \brief      读取最近一组三路转换，并解算角度
    \param[out] sample: 原码、角度和幅度
    \retval     none
*/
void hall_encoder_read(hall_sample_t *sample);

/*!
    \brief      复制最近一组三路 ADC 原码，不更新角度校准
    \param[out] raw: U1、U2、U4，各 12 位
    \retval     none
*/
void hall_encoder_copy_raw(uint16_t raw[3]);

/*!
    \brief      弱磁、校准有效、正在校准、有磁铁、饱和
    \param[in]  sample: 最近一次解算
    \retval     HALL_FLAG_* 的组合
*/
uint8_t hall_encoder_flags(const hall_sample_t *sample);

/*!
    \brief      装入已保存的校准，valid 为 0 时恢复成三路平均值
    \param[in]  cal: 偏置和增益
    \param[out] none
    \retval     none
*/
void hall_encoder_cal_apply(const hall_cal_t *cal);

/*!
    \brief      读出当前校准
    \param[out] cal: 偏置、增益和是否有效
    \retval     none
*/
void hall_encoder_cal_get(hall_cal_t *cal);

/*!
    \brief      开始记录三路最小/最大，转满一圈后再结束
    \param[in]  none
    \param[out] none
    \retval     none
*/
void hall_encoder_cal_begin(void);

/*!
    \brief      用记录到的峰峰值生成偏置和增益
    \param[in]  none
    \param[out] none
    \retval     HALL_CAL_OK、HALL_CAL_IDLE 或 HALL_CAL_WEAK
*/
uint8_t hall_encoder_cal_finish(void);

/*!
    \brief      丢掉校准，回到三路平均值
    \param[in]  none
    \param[out] none
    \retval     none
*/
void hall_encoder_cal_clear(void);

/*!
    \brief      设置要从原始角度里减掉的机械零点
    \param[in]  zero_cdeg: 0..35999
    \param[out] none
    \retval     none
*/
void hall_encoder_zero_set(uint16_t zero_cdeg);

/*!
    \brief      当前机械零点
    \param[in]  none
    \param[out] none
    \retval     0..35999，单位 0.01°
*/
uint16_t hall_encoder_zero_get(void);

/*!
    \brief      把此刻的角度记成 0°
    \param[in]  none
    \param[out] none
    \retval     1 成功，0 没有确认磁铁，零点不变
*/
uint8_t hall_encoder_zero_capture(void);

#endif /* HALL_ENCODER_H */
