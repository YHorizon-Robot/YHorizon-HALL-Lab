/*!
    \file    hall_line.h
    \brief   三颗等间距霍尔组成的直线位置

    物理顺序沿直线为 U1、U2、U4，间距 10 mm。U2 是 0，正方向指向 U4，
    所以 U1 在 -10 mm，U4 在 +10 mm。顺序和板子不一致时，只改下面三个下标。

    先减掉零磁场基线，再把绝对值最大的那一路翻成正峰。位置用峰和
    更强的相邻一路做对数高斯比值，敏感宽度要和磁铁高度一起调。
    结果写进 hall_sample_t.angle_cdeg，单位是 0.01 mm，不是角度。
*/

#ifndef HALL_LINE_H
#define HALL_LINE_H

#include "hall_encoder.h"

#define HALL_LINE_PITCH_UM          10000
#define HALL_LINE_SPAN_UM           15000
#define HALL_LINE_SIGMA_DEFAULT     8000U
#define HALL_LINE_SIGMA_MIN         2000U
#define HALL_LINE_SIGMA_MAX         30000U

/*! 原码下标：左、中、右。默认就是 U1、U2、U4 */
#define HALL_LINE_LEFT              0
#define HALL_LINE_MID               1
#define HALL_LINE_RIGHT             2

/*!
    \brief      装入基线、敏感宽度和机械零点
    valid 为 0 时基线用 2048，并清掉“已校准”标志。
    \param[in]  baseline: 三路零磁场原码
    \param[in]  valid: 非 0 表示基线可用
    \param[in]  sigma_um: 高斯宽度，单位 µm，超出范围时用默认 8 mm
    \param[in]  zero_cmm: 要从位置里减掉的零点，单位 0.01 mm
    \retval     none
*/
void hall_line_apply(const int16_t baseline[3], uint8_t valid,
                     uint16_t sigma_um, int16_t zero_cmm);

/*!
    \brief      读出当前基线、宽度和零点
    基线还没采过时，baseline 写成 0，valid 为 0。实际解算仍按 2048 去零。
    \param[out] baseline: 三路基线
    \param[out] valid: 基线是否有效
    \param[out] sigma_um: 敏感宽度，µm
    \param[out] zero_cmm: 机械零点，0.01 mm
    \retval     none
*/
void hall_line_get(int16_t baseline[3], uint8_t *valid,
                   uint16_t *sigma_um, int16_t *zero_cmm);

/*!
    \brief      修改敏感宽度，不改基线和零点
    \param[in]  sigma_um: 2000..30000 µm
    \retval     none
*/
void hall_line_sigma_set(uint16_t sigma_um);

/*!
    \brief      开始对零磁场取平均。磁铁要先拿开
    \retval     none
*/
void hall_line_cal_begin(void);

/*!
    \brief      用已经采到的平均值作为基线
    \retval     HALL_CAL_OK、HALL_CAL_IDLE 或 HALL_CAL_WEAK
*/
uint8_t hall_line_cal_finish(void);

/*!
    \brief      丢掉基线，回到 2048。零点和敏感宽度保留
    \retval     none
*/
void hall_line_cal_clear(void);

/*!
    \brief      设置要减掉的机械零点
    \param[in]  zero_cmm: 0.01 mm
    \retval     none
*/
void hall_line_zero_set(int16_t zero_cmm);

/*!
    \brief      当前机械零点
    \retval     0.01 mm
*/
int16_t hall_line_zero_get(void);

/*!
    \brief      把此刻的位置记成 0
    \retval     none
*/
void hall_line_zero_capture(void);

/*!
    \brief      读原码并解算位置
    angle_cdeg 在这里是有符号的 0.01 mm，amplitude 是正峰的 ADC 计数。
    \param[out] sample: 原码、位置和峰值
    \retval     none
*/
void hall_line_read(hall_sample_t *sample);

/*!
    \brief      弱磁、基线有效、正在采基线、超出阵列
    \param[in]  sample: 最近一次解算
    \retval     HALL_FLAG_* 的组合
*/
uint8_t hall_line_flags(const hall_sample_t *sample);

#endif /* HALL_LINE_H */
