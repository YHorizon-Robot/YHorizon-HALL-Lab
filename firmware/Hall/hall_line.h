/*!
    \file    hall_line.h
    \brief   三颗等间距霍尔组成的直线位置

    从下到上是 ADC1、ADC2、ADC3（原理图 U1、U2、U4）。相邻两颗相距 10 mm，
    ADC1 与 ADC3 相距 20 mm。ADC2 是 0，正方向朝上指向 ADC3，
    所以 ADC1 在 -10 mm，ADC3 在 +10 mm。顺序和板子不一致时，只改下面三个下标。

    每路减自己校准得到的空载。磁铁不在阵列上时基线才慢慢跟温漂，
    磁铁停在任何位置时基线不动，读数不会漂。
    主峰取被压低的那一路，旁边被抬高的回流不参与定位。行程拆成两段：
    ADC1 与 ADC2 算出 -10 mm 到 0，ADC2 与 ADC3 算出 0 到 +10 mm。
    每段用两路下拉量的对数比值，斜率由敏感宽度 sigma 决定；
    8 mm 磁铁离霍尔约 4 mm 时 sigma 约 3.4 mm。再按 ADC1、ADC3 的下拉量拼接。
    磁铁在 ADC1 上是 -10 mm，在 ADC3 上是 +10 mm，两端相距 20 mm。
    结果写进 hall_sample_t.angle_cdeg，单位是 0.01 mm，不是角度。
*/

#ifndef HALL_LINE_H
#define HALL_LINE_H

#include "hall_encoder.h"

#define HALL_LINE_PITCH_UM          10000
#define HALL_LINE_SPAN_UM           15000
#define HALL_LINE_SIGMA_DEFAULT     3400U
#define HALL_LINE_SIGMA_MIN         2000U
#define HALL_LINE_SIGMA_MAX         30000U

/*! 原码下标：下、中、上。默认就是 ADC1、ADC2、ADC3 */
#define HALL_LINE_BOTTOM            0
#define HALL_LINE_MID               1
#define HALL_LINE_TOP               2

/*!
    直线模式判断有磁铁，除了 hall_encoder.h 里的饱和和去抖，还要：
      - 已经校准过基线，否则 2048 和真实空载之差会被当成磁场；
      - 最深一路的下拉量过门限。8 mm 磁铁离霍尔 4 mm 时，
        正对一颗约 1200–1430，两颗中间约 375；
      - 最浅一路不超过最深一路的一半。均匀外磁场让三路一起变，会被这一条挡掉。
    磁铁抬高后中点幅度会变小，那时可以把门限往下调，但不要低于 80。
*/
#define HALL_LINE_MAG_ON            200
#define HALL_LINE_MAG_OFF           120

/*!
    \brief      装入基线、敏感宽度和机械零点
    valid 为 0 时基线用 2048，并清掉“已校准”标志。
    \param[in]  baseline: 三路零磁场原码
    \param[in]  valid: 非 0 表示基线可用
    \param[in]  sigma_um: 高斯宽度，单位 µm，超出范围时用默认 3.4 mm
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
    \brief      开始记录每路自己的空载
    磁铁拿开后停留，或在阵列上扫过都可以。出现最多的电平会当成这一路的基线。
    \retval     none
*/
void hall_line_cal_begin(void);

/*!
    \brief      用每路出现最多的那一团作为基线
    磁铁扫过的坑不参与。有一路全程被压着时不写入，校准继续留着。
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
    \retval     1 成功，0 没有确认磁铁，零点不变
*/
uint8_t hall_line_zero_capture(void);

/*!
    \brief      读原码并解算位置
    angle_cdeg 在这里是有符号的 0.01 mm，没有确认磁铁时是 HALL_NO_MAGNET。
    amplitude 是正峰的 ADC 计数，任何时候都如实给出，方便看门限。
    \param[out] sample: 原码、位置和峰值
    \retval     none
*/
void hall_line_read(hall_sample_t *sample);

/*!
    \brief      弱磁、基线有效、正在采基线、超出阵列、有磁铁、饱和
    \param[in]  sample: 最近一次解算
    \retval     HALL_FLAG_* 的组合
*/
uint8_t hall_line_flags(const hall_sample_t *sample);

#endif /* HALL_LINE_H */
