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

/*!
    \brief      一次采样结果
*/
typedef struct {
    uint16_t raw[3];       /*!< U1、U2、U4 的 12 位 ADC 原码 */
    int32_t angle_cdeg;    /*!< 0..35999，单位 0.01° */
    int32_t amplitude;     /*!< 单路交流峰值，单位 ADC 计数 */
} hall_sample_t;

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

#endif /* HALL_ENCODER_H */
