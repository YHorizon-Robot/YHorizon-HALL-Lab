/*!
    \file    hall_line.c
    \brief   直线霍尔阵列的基线、零点和对数高斯定位
*/

#include "hall_line.h"

#define HALL_LINE_COUNT             3
#define HALL_LINE_BASE_DEFAULT      2048
#define HALL_LINE_CAL_MIN           16U
#define HALL_LINE_CAL_MAX           4096U
#define HALL_LINE_LN2_Q16           45426

/*! log2(1 + i/32) 的 Q16，最后一项是 65536，所以用 32 位 */
static const uint32_t s_log2_q16[33] = {
    0U, 2909U, 5732U, 8473U, 11136U, 13727U, 16248U, 18704U,
    21098U, 23433U, 25711U, 27936U, 30109U, 32234U, 34312U, 36346U,
    38336U, 40286U, 42196U, 44068U, 45904U, 47705U, 49472U, 51207U,
    52911U, 54584U, 56229U, 57845U, 59434U, 60997U, 62534U, 64047U,
    65536U
};

static const int32_t s_pos_um[HALL_LINE_COUNT] = {
    -HALL_LINE_PITCH_UM, 0, HALL_LINE_PITCH_UM
};

static int16_t s_baseline[HALL_LINE_COUNT] = {
    HALL_LINE_BASE_DEFAULT, HALL_LINE_BASE_DEFAULT, HALL_LINE_BASE_DEFAULT
};
static uint16_t s_sigma_um = HALL_LINE_SIGMA_DEFAULT;
static int16_t s_zero_cmm;
static uint8_t s_valid;
static uint8_t s_cal_run;
static uint8_t s_out;
static int32_t s_cal_sum[HALL_LINE_COUNT];
static uint16_t s_cal_count;

/*!
    \brief      自然对数用的 log2，Q16
    \param[in]  value: 正数
    \retval     log2(value) * 65536
*/
static int32_t hall_line_log2_q16(uint32_t value)
{
    int32_t exp = 0;
    uint32_t index;
    uint32_t frac;
    uint32_t base;
    uint32_t span;

    if(0U == value) {
        return 0;
    }
    while(value > 65535U) {
        value >>= 1;
        exp++;
    }
    while(value < 32768U) {
        value <<= 1;
        exp--;
    }
    index = (value - 32768U) >> 10;
    frac = (value - 32768U) & 1023U;
    base = s_log2_q16[index];
    span = s_log2_q16[index + 1U] - base;
    return ((15 + exp) << 16) + (int32_t)base + (int32_t)((span * frac) >> 10);
}

/*!
    \brief      用峰和相邻一路估计磁铁位置
    \param[in]  raw: 三路原码
    \param[out] sample: 位置写在 angle_cdeg，单位 0.01 mm
    \retval     none
*/
static void hall_line_solve(const uint16_t raw[HALL_LINE_COUNT], hall_sample_t *sample)
{
    int32_t field[HALL_LINE_COUNT];
    int32_t peak_abs = -1;
    int32_t amp;
    int32_t neighbor = 0;
    int32_t pos_um;
    int32_t best = -1;
    uint32_t i;
    uint32_t peak = HALL_LINE_MID;

    s_out = 0U;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        int32_t mag;

        field[i] = (int32_t)raw[i] - (int32_t)s_baseline[i];
        sample->raw[i] = raw[i];
        mag = (field[i] < 0) ? -field[i] : field[i];
        if(mag > peak_abs) {
            peak_abs = mag;
            peak = i;
        }
    }
    if(field[peak] < 0) {
        for(i = 0U; i < HALL_LINE_COUNT; i++) {
            field[i] = -field[i];
        }
    }
    amp = field[peak];
    sample->amplitude = amp;
    if(amp <= 0) {
        sample->angle_cdeg = -(int32_t)s_zero_cmm;
        return;
    }

    if((peak > 0U) && (field[peak - 1U] > neighbor)) {
        best = (int32_t)peak - 1;
        neighbor = field[peak - 1U];
    }
    if((peak + 1U) < HALL_LINE_COUNT) {
        if(field[peak + 1U] > neighbor) {
            best = (int32_t)peak + 1;
            neighbor = field[peak + 1U];
        }
    }

    if((best < 0) || ((neighbor * 8) < amp)) {
        pos_um = s_pos_um[peak];
        if(HALL_LINE_MID != peak) {
            s_out = 1U;
        }
    } else {
        int32_t lns = hall_line_log2_q16((uint32_t)amp) - hall_line_log2_q16((uint32_t)neighbor);
        int32_t ln_q16 = (int32_t)(((int64_t)lns * HALL_LINE_LN2_Q16) / 65536);
        uint32_t scale = ((uint32_t)s_sigma_um * (uint32_t)s_sigma_um) / (uint32_t)HALL_LINE_PITCH_UM;
        int32_t delta = (int32_t)(((int64_t)scale * (int64_t)ln_q16) / 65536);
        int32_t mid = (s_pos_um[peak] + s_pos_um[best]) / 2;
        int32_t direction = (s_pos_um[peak] > s_pos_um[best]) ? 1 : -1;

        pos_um = mid + (direction * delta);
        /*! 三路几乎一样时没有梯度，比值会落到两颗传感器的中点。 */
        if(neighbor >= amp) {
            uint32_t third = 3U - peak - (uint32_t)best;

            if(field[third] >= neighbor) {
                pos_um = 0;
            }
        }
    }

    if(pos_um > HALL_LINE_SPAN_UM) {
        pos_um = HALL_LINE_SPAN_UM;
        s_out = 1U;
    } else if(pos_um < -HALL_LINE_SPAN_UM) {
        pos_um = -HALL_LINE_SPAN_UM;
        s_out = 1U;
    }
    sample->angle_cdeg = (pos_um / 10) - (int32_t)s_zero_cmm;
}

void hall_line_apply(const int16_t baseline[3], uint8_t valid,
                     uint16_t sigma_um, int16_t zero_cmm)
{
    uint32_t i;

    s_cal_run = 0U;
    s_cal_count = 0U;
    s_zero_cmm = zero_cmm;
    if((sigma_um < HALL_LINE_SIGMA_MIN) || (sigma_um > HALL_LINE_SIGMA_MAX)) {
        sigma_um = HALL_LINE_SIGMA_DEFAULT;
    }
    s_sigma_um = sigma_um;
    if(0U != valid) {
        s_valid = 1U;
        for(i = 0U; i < HALL_LINE_COUNT; i++) {
            s_baseline[i] = baseline[i];
        }
    } else {
        s_valid = 0U;
        for(i = 0U; i < HALL_LINE_COUNT; i++) {
            s_baseline[i] = HALL_LINE_BASE_DEFAULT;
        }
    }
}

void hall_line_get(int16_t baseline[3], uint8_t *valid,
                   uint16_t *sigma_um, int16_t *zero_cmm)
{
    uint32_t i;

    *valid = s_valid;
    *sigma_um = s_sigma_um;
    *zero_cmm = s_zero_cmm;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        baseline[i] = (0U != s_valid) ? s_baseline[i] : 0;
    }
}

void hall_line_sigma_set(uint16_t sigma_um)
{
    if((sigma_um < HALL_LINE_SIGMA_MIN) || (sigma_um > HALL_LINE_SIGMA_MAX)) {
        return;
    }
    s_sigma_um = sigma_um;
}

void hall_line_cal_begin(void)
{
    uint32_t i;

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        s_cal_sum[i] = 0;
    }
    s_cal_count = 0U;
    s_cal_run = 1U;
}

uint8_t hall_line_cal_finish(void)
{
    uint32_t i;

    if(0U == s_cal_run) {
        return HALL_CAL_IDLE;
    }
    if(s_cal_count < HALL_LINE_CAL_MIN) {
        return HALL_CAL_WEAK;
    }
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        s_baseline[i] = (int16_t)(s_cal_sum[i] / (int32_t)s_cal_count);
    }
    s_valid = 1U;
    s_cal_run = 0U;
    return HALL_CAL_OK;
}

void hall_line_cal_clear(void)
{
    uint32_t i;

    s_cal_run = 0U;
    s_cal_count = 0U;
    s_valid = 0U;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        s_baseline[i] = HALL_LINE_BASE_DEFAULT;
        s_cal_sum[i] = 0;
    }
}

void hall_line_zero_set(int16_t zero_cmm)
{
    s_zero_cmm = zero_cmm;
}

int16_t hall_line_zero_get(void)
{
    return s_zero_cmm;
}

void hall_line_zero_capture(void)
{
    hall_sample_t sample;
    int32_t raw_cmm;

    hall_line_read(&sample);
    raw_cmm = sample.angle_cdeg + (int32_t)s_zero_cmm;
    if(raw_cmm > 32767) {
        raw_cmm = 32767;
    }
    if(raw_cmm < -32768) {
        raw_cmm = -32768;
    }
    s_zero_cmm = (int16_t)raw_cmm;
}

void hall_line_read(hall_sample_t *sample)
{
    uint16_t raw[HALL_LINE_COUNT];
    uint32_t i;

    hall_encoder_copy_raw(raw);
    if((0U != s_cal_run) && (s_cal_count < HALL_LINE_CAL_MAX)) {
        for(i = 0U; i < HALL_LINE_COUNT; i++) {
            s_cal_sum[i] += (int32_t)raw[i];
        }
        s_cal_count++;
    }
    hall_line_solve(raw, sample);
}

uint8_t hall_line_flags(const hall_sample_t *sample)
{
    uint8_t flags = 0U;

    if(sample->amplitude < HALL_AMP_WEAK) {
        flags |= HALL_FLAG_WEAK;
    }
    if(0U != s_valid) {
        flags |= HALL_FLAG_CAL_VALID;
    }
    if(0U != s_cal_run) {
        flags |= HALL_FLAG_CAL_RUN;
    }
    if(0U != s_out) {
        flags |= HALL_FLAG_OUT;
    }
    return flags;
}
