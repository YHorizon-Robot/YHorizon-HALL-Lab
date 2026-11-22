/*!
    \file    hall_line.c
    \brief   直线霍尔阵列的基线、零点和分段对数比值定位
*/

#include "hall_line.h"

#include "systick.h"

#define HALL_LINE_COUNT             3
#define HALL_LINE_BASE_DEFAULT      2048
/*! 只跟温漂，时间常数取长一些 */
#define HALL_LINE_TRACK_MS          10000U
/*! 三路都离自己的基线这么近，才认为磁铁不在阵列上 */
#define HALL_LINE_TRACK_QUIET       12
/*! 对数比值里给两路各加的码数，邻路只剩噪声时不让 ln 发散 */
#define HALL_LINE_LOG_EPS           4U
#define HALL_LINE_LN2_Q16           45426
#define HALL_LINE_CAL_MIN           16U
/*! 每格 32 个码，覆盖 0..4095 */
#define HALL_LINE_CAL_BINS          128U
#define HALL_LINE_CAL_SHIFT         5U
/*! 样本多了就整表右移，让最近的空载压过更早的磁铁 */
#define HALL_LINE_CAL_DECAY         2048U
/*! 三路基线差过这么多，说明有一路一直被磁铁压着 */
#define HALL_LINE_CAL_SPREAD        800

/*! log2(1 + i/32) 的 Q16，最后一项是 65536，所以用 32 位 */
static const uint32_t s_log2_q16[33] = {
    0U, 2909U, 5732U, 8473U, 11136U, 13727U, 16248U, 18704U,
    21098U, 23433U, 25711U, 27936U, 30109U, 32234U, 34312U, 36346U,
    38336U, 40286U, 42196U, 44068U, 45904U, 47705U, 49472U, 51207U,
    52911U, 54584U, 56229U, 57845U, 59434U, 60997U, 62534U, 64047U,
    65536U
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
static hall_presence_t s_presence;
static uint8_t s_sat;
/*! 最浅一路不超过最深一路一半，说明磁场是局部的，不是均匀外磁场 */
static uint8_t s_local;

/*!
    \brief      丢掉基线跟踪的余量，避免刚装入的基线马上被拖走
    基线一换，之前的“有磁铁”判断也作废，要按新基线重新确认。
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
    hall_presence_reset(&s_presence);
}

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
    \brief      磁铁不在阵列上时，让基线慢慢跟上温漂
    只要有一路离基线超过 HALL_LINE_TRACK_QUIET，就认为磁铁还在，
    基线完全不动。这样磁铁停在任何位置，读数都不会被基线拖着走。
    还没校准时不跟，避免把 2048 和真实空载之间的差当成温漂。
    \param[in]  raw: 三路原码
    \param[out] none
    \retval     none
*/
static void hall_line_track(const uint16_t raw[HALL_LINE_COUNT])
{
    uint32_t now;
    uint32_t dt;
    uint32_t i;

    if((0U != s_cal_run) || (0U == s_valid)) {
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

    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        int32_t err = (int32_t)raw[i] - (int32_t)s_baseline[i];

        if((err > HALL_LINE_TRACK_QUIET) || (err < -HALL_LINE_TRACK_QUIET)) {
            for(i = 0U; i < HALL_LINE_COUNT; i++) {
                s_track_acc[i] = 0;
            }
            return;
        }
    }
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        int32_t err = (int32_t)raw[i] - (int32_t)s_baseline[i];
        int32_t step;

        s_track_acc[i] += err * (int32_t)dt;
        step = s_track_acc[i] / (int32_t)HALL_LINE_TRACK_MS;
        if(0 == step) {
            continue;
        }
        s_track_acc[i] -= step * (int32_t)HALL_LINE_TRACK_MS;
        s_baseline[i] = (int16_t)((int32_t)s_baseline[i] + step);
    }
}

/*!
    \brief      一段里两颗传感器的对数比值，得到这段内的位置
    假设磁场沿直线近似高斯，ln(近/远) 和位置成直线关系，
    宽度 sigma 就是这条直线的斜率，传感器正上方和中点都不会变平。
    \param[in]  d_mid: ADC2 的下拉量
    \param[in]  d_end: 这一段端点那一路的下拉量
    \retval     离 ADC2 的距离，0..pitch，单位 µm
*/
static int32_t hall_line_segment_um(int32_t d_mid, int32_t d_end)
{
    int32_t lns;
    int32_t ln_q16;
    uint32_t scale;
    int32_t pos;

    lns = hall_line_log2_q16((uint32_t)d_mid + HALL_LINE_LOG_EPS) -
          hall_line_log2_q16((uint32_t)d_end + HALL_LINE_LOG_EPS);
    ln_q16 = (int32_t)(((int64_t)lns * HALL_LINE_LN2_Q16) / 65536);
    scale = ((uint32_t)s_sigma_um * (uint32_t)s_sigma_um) / (uint32_t)HALL_LINE_PITCH_UM;
    pos = (HALL_LINE_PITCH_UM / 2) - (int32_t)(((int64_t)scale * ln_q16) / 65536);
    if(pos < 0) {
        pos = 0;
    }
    if(pos > HALL_LINE_PITCH_UM) {
        pos = HALL_LINE_PITCH_UM;
    }
    return pos;
}

/*!
    \brief      下段 ADC1–ADC2 与上段 ADC2–ADC3 分开算，再按两端下拉量拼接
    \param[in]  raw: 三路原码
    \param[out] sample: 位置写在 angle_cdeg，单位 0.01 mm
    \retval     none
*/
static void hall_line_solve(const uint16_t raw[HALL_LINE_COUNT], hall_sample_t *sample)
{
    int32_t field[HALL_LINE_COUNT];
    int32_t dip[HALL_LINE_COUNT];
    int32_t lowest = 0;
    int32_t highest = 0;
    int32_t amp = 0;
    int32_t x_lo;
    int32_t x_hi;
    int32_t den;
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
    lowest = field[0];
    for(i = 0U; i < HALL_LINE_COUNT; i++) {
        if(field[i] < lowest) {
            lowest = field[i];
        }
        dip[i] = field[i];
        if(dip[i] < 0) {
            dip[i] = 0;
        }
        if(dip[i] > amp) {
            amp = dip[i];
        }
    }
    s_local = ((lowest * 2) <= amp) ? 1U : 0U;
    sample->amplitude = amp;
    /*! 空载噪声会把两段比值甩开，幅度不够时固定在机械零点。 */
    if(amp < HALL_AMP_WEAK) {
        sample->angle_cdeg = -(int32_t)s_zero_cmm;
        return;
    }
    /*! 下段只看 ADC1 和 ADC2，结果在 -10 mm 到 0。 */
    x_lo = -hall_line_segment_um(dip[HALL_LINE_MID], dip[HALL_LINE_BOTTOM]);
    /*! 上段只看 ADC2 和 ADC3，结果在 0 到 +10 mm。 */
    x_hi = hall_line_segment_um(dip[HALL_LINE_MID], dip[HALL_LINE_TOP]);
    /*! 用两端谁被压得更低来拼接，经过 ADC2 时两段在 0 处相接。 */
    den = dip[HALL_LINE_BOTTOM] + dip[HALL_LINE_TOP];
    if(den <= 0) {
        pos_um = 0;
    } else {
        pos_um = (int32_t)(((int64_t)dip[HALL_LINE_BOTTOM] * x_lo +
                            (int64_t)dip[HALL_LINE_TOP] * x_hi) / den);
    }
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

uint8_t hall_line_zero_capture(void)
{
    hall_sample_t sample;
    int32_t raw_cmm;

    hall_line_read(&sample);
    if(0U == s_presence.present) {
        return 0U;
    }
    raw_cmm = sample.angle_cdeg + (int32_t)s_zero_cmm;
    if(raw_cmm > 32767) {
        raw_cmm = 32767;
    }
    if(raw_cmm < -32768) {
        raw_cmm = -32768;
    }
    s_zero_cmm = (int16_t)raw_cmm;
    return 1U;
}

void hall_line_read(hall_sample_t *sample)
{
    uint16_t raw[HALL_LINE_COUNT];
    uint8_t usable;
    uint8_t enter;
    uint8_t keep;

    hall_encoder_copy_raw(raw);
    if(0U != s_cal_run) {
        hall_line_cal_note(raw);
    }
    hall_line_track(raw);
    hall_line_solve(raw, sample);
    s_sat = hall_raw_saturated(raw);
    usable = ((0U != s_valid) && (0U == s_sat) && (0U != s_local)) ? 1U : 0U;
    enter = ((0U != usable) && (sample->amplitude >= HALL_LINE_MAG_ON)) ? 1U : 0U;
    keep = ((0U != usable) && (sample->amplitude >= HALL_LINE_MAG_OFF)) ? 1U : 0U;
    if(0U == hall_presence_update(&s_presence, enter, keep)) {
        sample->angle_cdeg = HALL_NO_MAGNET;
        s_out = 0U;
    }
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
    if(0U != s_presence.present) {
        flags |= HALL_FLAG_MAGNET;
    }
    if(0U != s_sat) {
        flags |= HALL_FLAG_SAT;
    }
    return flags;
}
