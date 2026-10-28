/*!
    \file    hall_line.c
    \brief   直线霍尔阵列的基线、零点和重心定位
*/

#include "hall_line.h"

#include "systick.h"

#define HALL_LINE_COUNT             3
#define HALL_LINE_BASE_DEFAULT      2048
/*! 磁铁已经离开时，基线跟上该路空载的时间常数 */
#define HALL_LINE_TRACK_FAST_MS     1000U
/*! 这一路正是最低的时候，可能是磁铁压着，只允许很慢地改基线 */
#define HALL_LINE_TRACK_SLOW_MS     120000U
/*! 三路彼此差这么多以内，认为磁铁不在阵列上 */
#define HALL_LINE_TRACK_QUIET       40
#define HALL_LINE_CAL_MIN           16U
/*! 每格 32 个码，覆盖 0..4095 */
#define HALL_LINE_CAL_BINS          128U
#define HALL_LINE_CAL_SHIFT         5U
/*! 样本多了就整表右移，让最近的空载压过更早的磁铁 */
#define HALL_LINE_CAL_DECAY         2048U
/*! 三路基线差过这么多，说明有一路一直被磁铁压着 */
#define HALL_LINE_CAL_SPREAD        800
/*! 只在 ±3 mm 内把过渡稍微拉开，区间外和线性重心相同 */
#define HALL_LINE_BLEND_UM          3000
#define HALL_LINE_BLEND_K_Q8        128

/*! ADC1、ADC2、ADC3 的位置。相邻 10 mm，ADC1 到 ADC3 为 20 mm */
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
static uint16_t s_cal_hist[HALL_LINE_COUNT][HALL_LINE_CAL_BINS];
static uint16_t s_cal_count;
static int32_t s_track_acc[HALL_LINE_COUNT];
static uint32_t s_track_ms;
static uint8_t s_track_init;

/*!
    \brief      丢掉基线跟踪的余量，避免刚装入的基线马上被拖走
    \param[in]  none
    \param[out] none
    \retval     none
*/
static void hall_line_track_reset(void)
{
    uint32_t i;

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        s_track_acc[i] = 0;
    }
    s_track_init = 0U;
}

/*!
    \brief      把每路基线收到它自己的空载
    偏低的那一路只在它不是当前最低时快速跟随，这样磁铁压低的坑
    不会被当成这颗霍尔的零点。三路已经很接近时，每一路都跟着走。
    \param[in]  raw: 三路原码
    \param[out] none
    \retval     none
*/
static void hall_line_track(const uint16_t raw[HALL_LINE_COUNT])
{
    uint32_t now;
    uint32_t dt;
    uint32_t i;
    uint32_t i_min;
    uint16_t raw_min;
    uint16_t raw_max;
    uint8_t quiet;

    if(0U != s_cal_run) {
        return;
    }
    now = systick_get_ms();
    if(0U == s_track_init) {
        s_track_ms = now;
        s_track_init = 1U;
        return;
    }
    dt = now - s_track_ms;
    if(0U == dt) {
        return;
    }
    if(dt > 500U) {
        dt = 500U;
    }
    s_track_ms = now;

    i_min = 0U;
    raw_min = raw[0];
    raw_max = raw[0];
    for(i = 1U; i < HALL_LINE_COUNT; i++) {
        if(raw[i] < raw_min) {
            raw_min = raw[i];
            i_min = i;
        }
        if(raw[i] > raw_max) {
            raw_max = raw[i];
        }
    }
    quiet = ((uint16_t)(raw_max - raw_min) <= HALL_LINE_TRACK_QUIET) ? 1U : 0U;

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        int32_t err = (int32_t)raw[i] - (int32_t)s_baseline[i];
        uint32_t tau = HALL_LINE_TRACK_FAST_MS;
        int32_t step;
        int32_t next;

        if((0U == quiet) && (i == i_min)) {
            tau = HALL_LINE_TRACK_SLOW_MS;
        }
        s_track_acc[i] += err * (int32_t)dt;
        step = s_track_acc[i] / (int32_t)tau;
        if(0 == step) {
            continue;
        }
        s_track_acc[i] -= step * (int32_t)tau;
        next = (int32_t)s_baseline[i] + step;
        if(next < 0) {
            next = 0;
        }
        if(next > 4095) {
            next = 4095;
        }
        s_baseline[i] = (int16_t)next;
        s_valid = 1U;
    }
}

/*!
    \brief      ±3 mm 内略微拉开，端点和斜率在边界上与原值重合
    \param[in]  pos_um: 线性重心，单位 µm
    \retval     过渡后的位置，单位 µm
*/
static int32_t hall_line_blend_center(int32_t pos_um)
{
    int32_t ax = pos_um;
    int32_t span;
    int32_t width2;
    int64_t bump;

    if(ax < 0) {
        ax = -ax;
    }
    if(ax >= HALL_LINE_BLEND_UM) {
        return pos_um;
    }
    width2 = HALL_LINE_BLEND_UM * HALL_LINE_BLEND_UM;
    span = width2 - (ax * ax);
    bump = (int64_t)span * (int64_t)span;
    bump /= width2;
    bump = bump * (int64_t)ax / width2;
    bump = bump * HALL_LINE_BLEND_K_Q8 / 256;
    if(pos_um < 0) {
        return pos_um - (int32_t)bump;
    }
    return pos_um + (int32_t)bump;
}

/*!
    \brief      用三路下拉量的重心估计磁铁位置
    \param[in]  raw: 三路原码
    \param[out] sample: 位置写在 angle_cdeg，单位 0.01 mm
    \retval     none
*/
static void hall_line_solve(const uint16_t raw[HALL_LINE_COUNT], hall_sample_t *sample)
{
    int32_t field[HALL_LINE_COUNT];
    int32_t lowest = 0;
    int32_t highest = 0;
    int32_t amp = 0;
    int32_t sum = 0;
    int64_t moment = 0;
    int32_t pos_um;
    uint32_t i;

    s_out = 0U;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        field[i] = (int32_t)raw[i] - (int32_t)s_baseline[i];
        sample->raw[i] = raw[i];
        if((0U == i) || (field[i] < lowest)) {
            lowest = field[i];
        }
        if((0U == i) || (field[i] > highest)) {
            highest = field[i];
        }
    }
    /*!
        轴向磁铁正对传感器时，最近的一路被压低，旁边的回流是抬高。
        压低不小于抬高时，把符号反过来，让被压低的那一路成为正的下拉量。
    */
    if((lowest < 0) && ((-lowest) >= highest)) {
        for(i = 0U; i < HALL_LINE_COUNT; i++) {
            field[i] = -field[i];
        }
    }
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        int32_t dip = field[i];

        if(dip < 0) {
            dip = 0;
        }
        if(dip > amp) {
            amp = dip;
        }
        sum += dip;
        moment += (int64_t)s_pos_um[i] * (int64_t)dip;
    }
    sample->amplitude = amp;
    /*! 空载噪声的重心会在整段行程里乱跳，幅度不够时固定在机械零点。 */
    if((sum <= 0) || (amp < HALL_AMP_WEAK)) {
        sample->angle_cdeg = -(int32_t)s_zero_cmm;
        return;
    }
    pos_um = hall_line_blend_center((int32_t)(moment / (int64_t)sum));
    if(pos_um > HALL_LINE_PITCH_UM) {
        pos_um = HALL_LINE_PITCH_UM;
    } else if(pos_um < -HALL_LINE_PITCH_UM) {
        pos_um = -HALL_LINE_PITCH_UM;
    }
    sample->angle_cdeg = (pos_um / 10) - (int32_t)s_zero_cmm;
}

void hall_line_apply(const int16_t baseline[3], uint8_t valid,
                     uint16_t sigma_um, int16_t zero_cmm)
{
    uint32_t i;

    s_cal_run = 0U;
    s_cal_count = 0U;
    hall_line_track_reset();
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
        baseline[i] = s_baseline[i];
    }
}

void hall_line_sigma_set(uint16_t sigma_um)
{
    if((sigma_um < HALL_LINE_SIGMA_MIN) || (sigma_um > HALL_LINE_SIGMA_MAX)) {
        return;
    }
    s_sigma_um = sigma_um;
}

/*!
    \brief      把直方图里出现最多的那一团换成基线码
    相邻格子计数不少于峰值的 1/4 才并进来，磁铁扫过时的过渡不会把空载拖偏。
    \param[in]  channel: 0..2
    \param[out] cluster: 这一团的样本数
    \retval     这一路的空载原码，没有样本时为 0
*/
static int32_t hall_line_cal_level(uint32_t channel, uint16_t *cluster)
{
    uint32_t bin;
    uint32_t best;
    uint32_t best_n;
    uint32_t left;
    uint32_t right;
    uint32_t count;
    uint32_t sum;
    const uint16_t *hist = s_cal_hist[channel];

    best = 0U;
    best_n = hist[0];
    for(bin = 1U; bin < HALL_LINE_CAL_BINS; bin++) {
        if(hist[bin] >= best_n) {
            best_n = hist[bin];
            best = bin;
        }
    }
    if(0U == best_n) {
        *cluster = 0U;
        return 0;
    }
    left = best;
    right = best;
    while((left > 0U) && (hist[left - 1U] != 0U) && ((hist[left - 1U] * 4U) >= best_n)) {
        left--;
    }
    while((right + 1U) < HALL_LINE_CAL_BINS) {
        if((hist[right + 1U] == 0U) || ((hist[right + 1U] * 4U) < best_n)) {
            break;
        }
        right++;
    }
    count = 0U;
    sum = 0U;
    for(bin = left; bin <= right; bin++) {
        uint32_t n = hist[bin];
        uint32_t center = (bin << HALL_LINE_CAL_SHIFT) + (1U << (HALL_LINE_CAL_SHIFT - 1U));

        sum += center * n;
        count += n;
    }
    *cluster = (count > 65535U) ? 65535U : (uint16_t)count;
    if(0U == count) {
        return 0;
    }
    return (int32_t)(sum / count);
}

/*!
    \brief      记一帧进直方图。样本多了就减半，最近的空载留得更久
    \param[in]  raw: 三路原码
    \retval     none
*/
static void hall_line_cal_note(const uint16_t raw[HALL_LINE_COUNT])
{
    uint32_t i;

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        uint32_t bin = (uint32_t)raw[i] >> HALL_LINE_CAL_SHIFT;

        if(bin >= HALL_LINE_CAL_BINS) {
            bin = HALL_LINE_CAL_BINS - 1U;
        }
        if(s_cal_hist[i][bin] < 65535U) {
            s_cal_hist[i][bin]++;
        }
    }
    if(s_cal_count < 65535U) {
        s_cal_count++;
    }
    if(s_cal_count < HALL_LINE_CAL_DECAY) {
        return;
    }
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        uint32_t bin;

        for(bin = 0U; bin < HALL_LINE_CAL_BINS; bin++) {
            s_cal_hist[i][bin] >>= 1;
        }
    }
    s_cal_count >>= 1;
}

void hall_line_cal_begin(void)
{
    uint32_t i;
    uint32_t bin;

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        for(bin = 0U; bin < HALL_LINE_CAL_BINS; bin++) {
            s_cal_hist[i][bin] = 0U;
        }
    }
    s_cal_count = 0U;
    s_cal_run = 1U;
    hall_line_track_reset();
}

uint8_t hall_line_cal_finish(void)
{
    int16_t baseline[HALL_LINE_COUNT];
    int16_t lowest;
    int16_t highest;
    uint32_t i;

    if(0U == s_cal_run) {
        return HALL_CAL_IDLE;
    }
    if(s_cal_count < HALL_LINE_CAL_MIN) {
        return HALL_CAL_WEAK;
    }
    lowest = 32767;
    highest = 0;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        uint16_t cluster = 0U;
        int32_t level = hall_line_cal_level(i, &cluster);

        if(cluster < HALL_LINE_CAL_MIN) {
            return HALL_CAL_WEAK;
        }
        baseline[i] = (int16_t)level;
        if(baseline[i] < lowest) {
            lowest = baseline[i];
        }
        if(baseline[i] > highest) {
            highest = baseline[i];
        }
    }
    /*! 某一路全程被磁铁压着时，它的众数会离另外两路很远，先不写入。 */
    if((int32_t)highest - (int32_t)lowest > HALL_LINE_CAL_SPREAD) {
        return HALL_CAL_WEAK;
    }
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        s_baseline[i] = baseline[i];
    }
    s_valid = 1U;
    s_cal_run = 0U;
    hall_line_track_reset();
    return HALL_CAL_OK;
}

void hall_line_cal_clear(void)
{
    uint32_t i;

    s_cal_run = 0U;
    s_cal_count = 0U;
    hall_line_track_reset();
    s_valid = 0U;
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        uint32_t bin;

        s_baseline[i] = HALL_LINE_BASE_DEFAULT;
        for(bin = 0U; bin < HALL_LINE_CAL_BINS; bin++) {
            s_cal_hist[i][bin] = 0U;
        }
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

    hall_encoder_copy_raw(raw);
    if(0U != s_cal_run) {
        hall_line_cal_note(raw);
    }
    hall_line_track(raw);
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
