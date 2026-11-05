/*!
    \file    hall_encoder.c
    \brief   读取 U1/U2/U4，并把 120° 三相信号合成磁铁角度

    GD32E230 只有一个 ADC。三路放入规则组扫描，DMA 通道 0 循环写入。
    传感器是 SC4703，零磁场输出约为 VDD/2，所以零点用三路平均值去掉，
    不依赖固定的 2048。
*/

#include "hall_encoder.h"

#include "gd32e23x.h"
#include "systick.h"

#define HALL_ADC_COUNT              3U
#define HALL_SQRT3_NUM              887
#define HALL_SQRT3_SHIFT            9

static volatile uint16_t s_adc_dma[HALL_ADC_COUNT];
static hall_cal_t s_cal = {
    {0, 0, 0},
    {HALL_CAL_GAIN_ONE, HALL_CAL_GAIN_ONE, HALL_CAL_GAIN_ONE},
    0U
};
static uint16_t s_zero_cdeg;
static int32_t s_angle_raw;
static uint8_t s_cal_run;
static uint16_t s_cal_min[HALL_ADC_COUNT];
static uint16_t s_cal_max[HALL_ADC_COUNT];
static hall_presence_t s_presence;
static uint8_t s_sat;

uint8_t hall_presence_update(hall_presence_t *p, uint8_t enter, uint8_t keep)
{
    uint32_t now = systick_get_ms();
    uint8_t want_change;
    uint32_t hold;

    if(0U != p->present) {
        want_change = (0U == keep) ? 1U : 0U;
        hold = HALL_MAG_OFF_MS;
    } else {
        want_change = (0U != enter) ? 1U : 0U;
        hold = HALL_MAG_ON_MS;
    }
    if(0U == want_change) {
        p->timing = 0U;
        return p->present;
    }
    if(0U == p->timing) {
        p->timing = 1U;
        p->since_ms = now;
        return p->present;
    }
    if((now - p->since_ms) >= hold) {
        p->present = (0U != p->present) ? 0U : 1U;
        p->timing = 0U;
    }
    return p->present;
}

void hall_presence_reset(hall_presence_t *p)
{
    p->present = 0U;
    p->timing = 0U;
    p->since_ms = 0U;
}

uint8_t hall_raw_saturated(const uint16_t raw[3])
{
    uint32_t i;

    for(i = 0U; i < 3U; i++) {
        if((raw[i] <= HALL_RAW_SAT_LO) || (raw[i] >= HALL_RAW_SAT_HI)) {
            return 1U;
        }
    }
    return 0U;
}

/*!
    \brief      把未使用的引脚设成模拟输入，降低悬空输入的漏电
    \param[in]  none
    \param[out] none
    \retval     none
*/
static void hall_unused_pins_analog(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOF);

    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE,
                  GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_5 | GPIO_PIN_6 |
                  GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_15);
    gpio_mode_set(GPIOB, GPIO_MODE_ANALOG, GPIO_PUPD_NONE,
                  GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_6 | GPIO_PIN_7);
    gpio_mode_set(GPIOF, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_0 | GPIO_PIN_1);
}

/*!
    \brief      PA0、PA1、PA4 扫描转换，结果由 DMA 循环刷新
    \param[in]  none
    \param[out] none
    \retval     none
*/
static void hall_adc_dma_init(void)
{
    dma_parameter_struct dma;

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_ADC);
    rcu_periph_clock_enable(RCU_DMA);
    /*!
        SystemInit 会关掉 IRC28M。ADC 时钟源选到它之前必须重新打开，
        否则校准停在 CLB 标志上，主循环里的 RTT 日志不会发出去。
    */
    rcu_osci_on(RCU_IRC28M);
    if(SUCCESS == rcu_osci_stab_wait(RCU_IRC28M)) {
        rcu_adc_clock_config(RCU_ADCCK_IRC28M_DIV2);
    } else {
        rcu_adc_clock_config(RCU_ADCCK_APB2_DIV4);
    }

    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE,
                  GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4);

    dma_deinit(DMA_CH0);
    dma_struct_para_init(&dma);
    dma.periph_addr = (uint32_t)(&ADC_RDATA);
    dma.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    dma.memory_addr = (uint32_t)s_adc_dma;
    dma.memory_width = DMA_MEMORY_WIDTH_16BIT;
    dma.number = HALL_ADC_COUNT;
    dma.priority = DMA_PRIORITY_HIGH;
    dma.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    dma.direction = DMA_PERIPHERAL_TO_MEMORY;
    dma_init(DMA_CH0, &dma);
    dma_circulation_enable(DMA_CH0);
    dma_channel_enable(DMA_CH0);

    adc_deinit();
    adc_special_function_config(ADC_SCAN_MODE, ENABLE);
    adc_special_function_config(ADC_CONTINUOUS_MODE, ENABLE);
    adc_data_alignment_config(ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(ADC_REGULAR_CHANNEL, HALL_ADC_COUNT);
    adc_regular_channel_config(0U, ADC_CHANNEL_0, ADC_SAMPLETIME_55POINT5);
    adc_regular_channel_config(1U, ADC_CHANNEL_1, ADC_SAMPLETIME_55POINT5);
    adc_regular_channel_config(2U, ADC_CHANNEL_4, ADC_SAMPLETIME_55POINT5);
    adc_external_trigger_source_config(ADC_REGULAR_CHANNEL, ADC_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC_REGULAR_CHANNEL, ENABLE);
    /*!
        每路连续转换 64 次再右移 6 位，结果仍是 12 位满量程。
        SC4703 输出噪声大约 9 mV，单次采样会晃十几个码；64 次平均后大约降到 1～2 个码。
    */
    adc_oversample_mode_config(ADC_OVERSAMPLING_ALL_CONVERT,
                               ADC_OVERSAMPLING_SHIFT_6B,
                               ADC_OVERSAMPLING_RATIO_MUL64);
    adc_oversample_mode_enable();
    adc_dma_mode_enable();

    adc_enable();
    delay_1ms(1U);
    adc_calibration_enable();
    adc_software_trigger_enable(ADC_REGULAR_CHANNEL);
}

/*!
    \brief      初始化霍尔采样
    \param[in]  none
    \param[out] none
    \retval     none
*/
void hall_encoder_init(void)
{
    hall_unused_pins_analog();
    hall_adc_dma_init();
}

/*!
    \brief      整数平方根
    \param[in]  value: 被开方数
    \retval     floor(sqrt(value))
*/
static uint32_t hall_isqrt(uint32_t value)
{
    uint32_t op = value;
    uint32_t res = 0U;
    uint32_t one = 1UL << 30;

    while(one > op) {
        one >>= 2;
    }
    while(0U != one) {
        if(op >= (res + one)) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return res;
}

/*!
    \brief      atan2 近似，返回 -18000..18000，单位 0.01°
    \param[in]  y: 正弦分量
    \param[in]  x: 余弦分量
    \retval     角度，0.01°
*/
static int32_t hall_atan2_cdeg(int32_t y, int32_t x)
{
    uint32_t ax;
    uint32_t ay;
    uint32_t a;
    uint32_t b;
    uint32_t z;
    uint32_t acute;
    int32_t angle;

    if((0 == x) && (0 == y)) {
        return 0;
    }

    ax = (x < 0) ? (uint32_t)(-x) : (uint32_t)x;
    ay = (y < 0) ? (uint32_t)(-y) : (uint32_t)y;
    if(ax >= ay) {
        a = ay;
        b = ax;
    } else {
        a = ax;
        b = ay;
    }
    if(0U == b) {
        return 0;
    }

    z = (uint32_t)(((uint64_t)a << 16) / b);
    if(z > 65536U) {
        z = 65536U;
    }
    acute = (z * 4500U) >> 16;
    acute += (uint32_t)(((uint64_t)z * (65536U - z) * 1564U) >> 32);
    if(ay > ax) {
        acute = 9000U - acute;
    }

    angle = (int32_t)acute;
    if(x < 0) {
        angle = 18000 - angle;
    }
    if(y < 0) {
        angle = -angle;
    }
    return angle;
}

/*!
    \brief      由三路原码计算角度和幅度
    \param[in]  raw: U1、U2、U4
    \param[out] sample: 原码、0..35999 的角度、单路峰值
    \retval     none
*/
static void hall_solve(const uint16_t raw[3], hall_sample_t *sample)
{
    int32_t centered[3];
    int32_t mean;
    int32_t s0;
    int32_t s1;
    int32_t s2;
    int32_t alpha;
    int32_t beta;
    int32_t angle;
    uint64_t mag2;
    uint32_t i;

    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        sample->raw[i] = raw[i];
        if(0U != s_cal.valid) {
            centered[i] = (int32_t)raw[i] - (int32_t)s_cal.offset[i];
            centered[i] = (int32_t)(((int64_t)centered[i] * (int32_t)s_cal.gain_q12[i]) >> 12);
        } else {
            centered[i] = (int32_t)raw[i];
        }
    }
    mean = (centered[0] + centered[1] + centered[2]) / 3;
    centered[0] -= mean;
    centered[1] -= mean;
    centered[2] -= mean;

    s0 = centered[HALL_PHASE_0_DEG];
    s1 = centered[HALL_PHASE_120_DEG];
    s2 = centered[HALL_PHASE_240_DEG];

    /* alpha = 3*A*cos(θ)，beta = 3*A*sin(θ)。√3 ≈ 887/512。 */
    alpha = (2 * s0) - s1 - s2;
    beta = (int32_t)(((int64_t)(s1 - s2) * HALL_SQRT3_NUM) >> HALL_SQRT3_SHIFT);
    beta *= HALL_ANGLE_SIGN;

    angle = hall_atan2_cdeg(beta, alpha);
    if(angle < 0) {
        angle += 36000;
    }
    s_angle_raw = angle;
    if(angle < (int32_t)s_zero_cdeg) {
        angle += 36000;
    }
    angle -= (int32_t)s_zero_cdeg;
    sample->angle_cdeg = angle;

    mag2 = ((int64_t)alpha * alpha) + ((int64_t)beta * beta);
    if(mag2 > 0xFFFFFFFFULL) {
        mag2 = 0xFFFFFFFFULL;
    }
    /* 矢量长度是 3*A，除以 3 得到单路峰值。 */
    sample->amplitude = (int32_t)(hall_isqrt((uint32_t)mag2) / 3U);
}

/*!
    \brief      读取 DMA 缓冲区并解算
    \param[out] sample: 本次结果
    \retval     none
*/
/*!
    \brief      校准过程中记下三路原码的最小和最大
    \param[in]  raw: U1、U2、U4
    \param[out] none
    \retval     none
*/
static void hall_cal_note(const uint16_t raw[3])
{
    uint32_t i;

    if(0U == s_cal_run) {
        return;
    }
    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        if(raw[i] < s_cal_min[i]) {
            s_cal_min[i] = raw[i];
        }
        if(raw[i] > s_cal_max[i]) {
            s_cal_max[i] = raw[i];
        }
    }
}

void hall_encoder_copy_raw(uint16_t raw[3])
{
    raw[0] = s_adc_dma[0];
    raw[1] = s_adc_dma[1];
    raw[2] = s_adc_dma[2];
}

void hall_encoder_read(hall_sample_t *sample)
{
    uint16_t raw[HALL_ADC_COUNT];
    uint8_t enter;
    uint8_t keep;

    hall_encoder_copy_raw(raw);
    hall_cal_note(raw);
    hall_solve(raw, sample);
    /*!
        三路共模已经减掉，外界均匀磁场不会产生幅度。幅度只来自转动磁铁
        的三相分量，或者未校准时三路偏置之差，后者远低于进入门限。
    */
    s_sat = hall_raw_saturated(raw);
    enter = ((0U == s_sat) && (sample->amplitude >= HALL_ENC_MAG_ON)) ? 1U : 0U;
    keep = ((0U == s_sat) && (sample->amplitude >= HALL_ENC_MAG_OFF)) ? 1U : 0U;
    if(0U == hall_presence_update(&s_presence, enter, keep)) {
        sample->angle_cdeg = HALL_NO_MAGNET;
    }
}

uint8_t hall_encoder_flags(const hall_sample_t *sample)
{
    uint8_t flags = 0U;

    if(sample->amplitude < HALL_AMP_WEAK) {
        flags |= HALL_FLAG_WEAK;
    }
    if(0U != s_cal.valid) {
        flags |= HALL_FLAG_CAL_VALID;
    }
    if(0U != s_cal_run) {
        flags |= HALL_FLAG_CAL_RUN;
    }
    if(0U != s_presence.present) {
        flags |= HALL_FLAG_MAGNET;
    }
    if(0U != s_sat) {
        flags |= HALL_FLAG_SAT;
    }
    return flags;
}

void hall_encoder_cal_apply(const hall_cal_t *cal)
{
    uint32_t i;

    if(0U == cal->valid) {
        hall_encoder_cal_clear();
        return;
    }
    s_cal.valid = 1U;
    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        s_cal.offset[i] = cal->offset[i];
        if(0U == cal->gain_q12[i]) {
            s_cal.gain_q12[i] = HALL_CAL_GAIN_ONE;
        } else {
            s_cal.gain_q12[i] = cal->gain_q12[i];
        }
    }
}

void hall_encoder_cal_get(hall_cal_t *cal)
{
    *cal = s_cal;
}

void hall_encoder_cal_begin(void)
{
    uint32_t i;

    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        s_cal_min[i] = 4095U;
        s_cal_max[i] = 0U;
    }
    s_cal_run = 1U;
}

uint8_t hall_encoder_cal_finish(void)
{
    uint32_t i;
    uint16_t span[HALL_ADC_COUNT];
    int16_t offset[HALL_ADC_COUNT];
    uint16_t gain_q12[HALL_ADC_COUNT];
    uint32_t gain;

    if(0U == s_cal_run) {
        return HALL_CAL_IDLE;
    }
    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        if(s_cal_max[i] <= s_cal_min[i]) {
            return HALL_CAL_WEAK;
        }
        span[i] = (uint16_t)(s_cal_max[i] - s_cal_min[i]);
        if(span[i] < HALL_CAL_SPAN_MIN) {
            return HALL_CAL_WEAK;
        }
        offset[i] = (int16_t)(((uint32_t)s_cal_min[i] + (uint32_t)s_cal_max[i]) / 2U);
        gain = ((uint32_t)span[0] << 12) / (uint32_t)span[i];
        if((0U == gain) || (gain > 65535U)) {
            return HALL_CAL_WEAK;
        }
        gain_q12[i] = (uint16_t)gain;
    }
    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        s_cal.offset[i] = offset[i];
        s_cal.gain_q12[i] = gain_q12[i];
    }
    s_cal.valid = 1U;
    s_cal_run = 0U;
    return HALL_CAL_OK;
}

void hall_encoder_cal_clear(void)
{
    uint32_t i;

    s_cal_run = 0U;
    s_cal.valid = 0U;
    for(i = 0U; i < HALL_ADC_COUNT; i++) {
        s_cal.offset[i] = 0;
        s_cal.gain_q12[i] = HALL_CAL_GAIN_ONE;
        s_cal_min[i] = 4095U;
        s_cal_max[i] = 0U;
    }
}

void hall_encoder_zero_set(uint16_t zero_cdeg)
{
    if(zero_cdeg < 36000U) {
        s_zero_cdeg = zero_cdeg;
    }
}

uint16_t hall_encoder_zero_get(void)
{
    return s_zero_cdeg;
}

uint8_t hall_encoder_zero_capture(void)
{
    hall_sample_t sample;

    hall_encoder_read(&sample);
    if(0U == s_presence.present) {
        return 0U;
    }
    s_zero_cdeg = (uint16_t)s_angle_raw;
    return 1U;
}
