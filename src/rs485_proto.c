/*!
    \file    rs485_proto.c
    \brief   RS485 收发、参数保存和霍尔命令
*/

#include "rs485_proto.h"

#include "gd32e23x.h"
#include "hall_encoder.h"
#include "hall_line.h"
#include "systick.h"

#include <stddef.h>
#include <string.h>

#define RS485_PAYLOAD_MAX           32U
#define RS485_FRAME_MAX             (10U + RS485_PAYLOAD_MAX)
#define RS485_RX_SIZE               128U
#define RS485_RX_MASK               (RS485_RX_SIZE - 1U)
#define RS485_GAP_MS                20U
#define RS485_QUIET_MS              3U
#define RS485_REPORT_MIN_MS         5U
#define RS485_REPORT_MAX_MS         60000U
#define RS485_CFG_MAGIC             0x314C4C48UL
#define RS485_CFG_LAYOUT            1U
#define RS485_CFG_ADDR              0x08007C00UL

/*!
    \brief      掉电保存的参数，放在最后一页 Flash
*/
typedef struct {
    uint32_t magic;
    uint16_t layout;
    uint16_t crc;
    uint8_t id;
    uint8_t mode;
    uint8_t baud_index;
    uint8_t cal_valid;
    uint16_t report_ms;
    uint16_t zero_cdeg;
    int16_t offset[3];
    uint16_t gain_q12[3];
    uint16_t profile;          /*!< 0 或 1 为角度，2 为直线。旧固件这里是 0 */
    int16_t line_zero_cmm;     /*!< 直线零点，0.01 mm。旧固件这里是 0 */
} rs485_cfg_t;

_Static_assert(sizeof(rs485_cfg_t) == 32U, "rs485 config must be 32 bytes");
_Static_assert(offsetof(rs485_cfg_t, crc) == 6U, "crc follows magic and layout");
_Static_assert(offsetof(rs485_cfg_t, profile) == 28U, "profile stays in the old reserved slot");
_Static_assert((RS485_RX_SIZE & RS485_RX_MASK) == 0U, "rx ring is a power of two");

static const uint32_t s_baud_table[RS485_BAUD_COUNT] = {
    9600UL, 19200UL, 38400UL, 57600UL, 115200UL, 230400UL, 460800UL, 921600UL
};

static rs485_cfg_t s_cfg;
static volatile uint8_t s_rx[RS485_RX_SIZE];
static volatile uint8_t s_rx_w;
static volatile uint8_t s_rx_r;
static uint8_t s_asm[RS485_FRAME_MAX];
static uint8_t s_asm_len;
static uint32_t s_asm_ms;
static uint32_t s_last_rx_ms;
static uint32_t s_last_tx_ms;
static uint32_t s_report_ms;
static uint8_t s_report_seq;
static uint8_t s_reinit_uart;

/*!
    \brief      CRC-16/MODBUS
    \param[in]  data: 数据
    \param[in]  len: 字节数
    \retval     校验值
*/
static uint16_t rs485_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t bit;

    for(i = 0U; i < len; i++) {
        crc ^= data[i];
        for(bit = 0U; bit < 8U; bit++) {
            if(0U != (crc & 0x0001U)) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/*!
    \brief      大约等待若干微秒，只用于方向脚翻转
    \param[in]  us: 微秒
    \retval     none
*/
static void rs485_delay_us(uint32_t us)
{
    uint32_t n = (SystemCoreClock / 4000000U) * us;

    while(0U != n) {
        __NOP();
        n--;
    }
}

static void rs485_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static uint16_t rs485_get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void rs485_put_i16(uint8_t *p, int32_t value)
{
    if(value > 32767) {
        value = 32767;
    }
    if(value < -32768) {
        value = -32768;
    }
    rs485_put_u16(p, (uint16_t)(int16_t)value);
}

static void rs485_put_i32(uint8_t *p, int32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)((uint32_t)value >> 8);
    p[2] = (uint8_t)((uint32_t)value >> 16);
    p[3] = (uint8_t)((uint32_t)value >> 24);
}

/*!
    \brief      恢复未写过 Flash 时的默认参数
    \retval     none
*/
static void rs485_cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic = RS485_CFG_MAGIC;
    s_cfg.layout = RS485_CFG_LAYOUT;
    s_cfg.id = 1U;
    s_cfg.mode = RS485_MODE_POLL;
    s_cfg.baud_index = RS485_BAUD_115200;
    s_cfg.report_ms = 20U;
    s_cfg.gain_q12[0] = HALL_CAL_GAIN_ONE;
    s_cfg.gain_q12[1] = HALL_CAL_GAIN_ONE;
    s_cfg.gain_q12[2] = HALL_CAL_GAIN_ONE;
}

/*!
    \brief      当前是不是直线位置模式
    \retval     1 直线，0 角度
*/
static uint8_t rs485_is_line(void)
{
    return (RS485_TYPE_HALL_POSITION == s_cfg.profile) ? 1U : 0U;
}

/*!
    \brief      对外报告的设备类型。旧参数里的 0 仍是角度编码器
    \retval     RS485_TYPE_HALL_ENCODER 或 RS485_TYPE_HALL_POSITION
*/
static uint8_t rs485_device_type(void)
{
    if(0U != rs485_is_line()) {
        return RS485_TYPE_HALL_POSITION;
    }
    return RS485_TYPE_HALL_ENCODER;
}

/*!
    \brief      把正在使用的零点和校准抄进参数，不动另一种模式的槽位
    \retval     none
*/
static void rs485_cfg_pull_hall(void)
{
    uint32_t i;

    if(0U != rs485_is_line()) {
        int16_t baseline[3];
        uint8_t valid;
        uint16_t sigma;
        int16_t zero;

        hall_line_get(baseline, &valid, &sigma, &zero);
        s_cfg.cal_valid = valid;
        s_cfg.line_zero_cmm = zero;
        s_cfg.gain_q12[0] = sigma;
        for(i = 0U; i < 3U; i++) {
            s_cfg.offset[i] = baseline[i];
        }
        return;
    }

    {
        hall_cal_t cal;

        hall_encoder_cal_get(&cal);
        s_cfg.cal_valid = cal.valid;
        s_cfg.zero_cdeg = hall_encoder_zero_get();
        for(i = 0U; i < 3U; i++) {
            s_cfg.offset[i] = cal.offset[i];
            s_cfg.gain_q12[i] = cal.gain_q12[i];
        }
    }
}

/*!
    \brief      把参数装进当前模式。直线模式的宽度放在 gain_q12[0]
    \retval     none
*/
static void rs485_cfg_push_hall(void)
{
    uint32_t i;

    if(0U != rs485_is_line()) {
        hall_line_apply(s_cfg.offset, s_cfg.cal_valid, s_cfg.gain_q12[0], s_cfg.line_zero_cmm);
        return;
    }

    {
        hall_cal_t cal;

        memset(&cal, 0, sizeof(cal));
        cal.valid = s_cfg.cal_valid;
        for(i = 0U; i < 3U; i++) {
            cal.offset[i] = s_cfg.offset[i];
            cal.gain_q12[i] = s_cfg.gain_q12[i];
            if(0U == cal.gain_q12[i]) {
                cal.gain_q12[i] = HALL_CAL_GAIN_ONE;
                cal.valid = 0U;
            }
        }
        hall_encoder_cal_apply(&cal);
        hall_encoder_zero_set(s_cfg.zero_cdeg);
    }
}

/*!
    \brief      换成另一种测量，并清掉两边的零点和校准
    \param[in]  type: RS485_TYPE_HALL_ENCODER 或 RS485_TYPE_HALL_POSITION
    \retval     none
*/
static void rs485_enter_profile(uint8_t type)
{
    s_cfg.profile = (RS485_TYPE_HALL_POSITION == type) ? RS485_TYPE_HALL_POSITION : 0U;
    s_cfg.cal_valid = 0U;
    s_cfg.zero_cdeg = 0U;
    s_cfg.line_zero_cmm = 0;
    s_cfg.offset[0] = 0;
    s_cfg.offset[1] = 0;
    s_cfg.offset[2] = 0;
    hall_encoder_cal_clear();
    hall_encoder_zero_set(0U);
    hall_line_cal_clear();
    hall_line_zero_set(0);
    if(0U != rs485_is_line()) {
        s_cfg.gain_q12[0] = HALL_LINE_SIGMA_DEFAULT;
        s_cfg.gain_q12[1] = 0U;
        s_cfg.gain_q12[2] = 0U;
    } else {
        s_cfg.gain_q12[0] = HALL_CAL_GAIN_ONE;
        s_cfg.gain_q12[1] = HALL_CAL_GAIN_ONE;
        s_cfg.gain_q12[2] = HALL_CAL_GAIN_ONE;
    }
    rs485_cfg_push_hall();
}

/*!
    \brief      参数是否能用
    \param[in]  cfg: Flash 里读出的副本
    \retval     1 可用，0 丢弃
*/
static uint8_t rs485_cfg_sane(const rs485_cfg_t *cfg)
{
    rs485_cfg_t img;
    uint16_t crc;

    if((RS485_CFG_MAGIC != cfg->magic) || (RS485_CFG_LAYOUT != cfg->layout)) {
        return 0U;
    }
    if((0U == cfg->id) || (cfg->id >= RS485_ADDR_BCAST)) {
        return 0U;
    }
    if(cfg->mode > RS485_MODE_REPORT) {
        return 0U;
    }
    if(cfg->baud_index >= RS485_BAUD_COUNT) {
        return 0U;
    }
    if((cfg->report_ms < RS485_REPORT_MIN_MS) || (cfg->report_ms > RS485_REPORT_MAX_MS)) {
        return 0U;
    }
    if(cfg->zero_cdeg >= 36000U) {
        return 0U;
    }
    if((0U != cfg->profile) &&
            (RS485_TYPE_HALL_ENCODER != cfg->profile) &&
            (RS485_TYPE_HALL_POSITION != cfg->profile)) {
        return 0U;
    }
    img = *cfg;
    img.crc = 0U;
    crc = rs485_crc16((const uint8_t *)&img, (uint16_t)sizeof(img));
    return (crc == cfg->crc) ? 1U : 0U;
}

/*!
    \brief      擦除并写入最后一页
    \retval     0 成功，-1 失败
*/
static int rs485_cfg_save(void)
{
    rs485_cfg_t img;
    const uint32_t *word;
    uint32_t primask;
    uint32_t i;
    int ok = 0;

    rs485_cfg_pull_hall();
    img = s_cfg;
    img.magic = RS485_CFG_MAGIC;
    img.layout = RS485_CFG_LAYOUT;
    img.crc = 0U;
    img.crc = rs485_crc16((const uint8_t *)&img, (uint16_t)sizeof(img));

    primask = __get_PRIMASK();
    __disable_irq();
    fmc_unlock();
    fmc_flag_clear(FMC_FLAG_END | FMC_FLAG_WPERR | FMC_FLAG_PGERR | FMC_FLAG_PGAERR);
    if(FMC_READY == fmc_page_erase(RS485_CFG_ADDR)) {
        word = (const uint32_t *)&img;
        ok = 1;
        for(i = 0U; i < (sizeof(img) / 4U); i++) {
            if(FMC_READY != fmc_word_program(RS485_CFG_ADDR + (i * 4U), word[i])) {
                ok = 0;
                break;
            }
        }
    }
    fmc_lock();
    if(0U == primask) {
        __enable_irq();
    }
    if(0 != ok) {
        s_cfg.crc = img.crc;
        return 0;
    }
    return -1;
}

/*!
    \brief      读 Flash，校验失败就用默认值，不立刻回写
    \retval     none
*/
static void rs485_cfg_load(void)
{
    rs485_cfg_t img;

    memcpy(&img, (const void *)RS485_CFG_ADDR, sizeof(img));
    if(0U != rs485_cfg_sane(&img)) {
        s_cfg = img;
    } else {
        rs485_cfg_defaults();
    }
    rs485_cfg_push_hall();
}

/*!
    \brief      切换波特率，引脚和中断保持不变
    \param[in]  baud: bit/s
    \retval     none
*/
static void rs485_uart_baud(uint32_t baud)
{
    usart_disable(USART0);
    usart_baudrate_set(USART0, baud);
    usart_enable(USART0);
    usart_interrupt_enable(USART0, USART_INT_RBNE);
}

/*!
    \brief      配置 PA9/PA10 和 USART0
    \retval     none
*/
static void rs485_uart_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_USART0);
    rcu_usart_clock_config(RCU_USART0SRC_CKAPB2);

    gpio_mode_set(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_5);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_5);
    gpio_bit_reset(GPIOB, GPIO_PIN_5);

    gpio_af_set(GPIOA, GPIO_AF_1, GPIO_PIN_9 | GPIO_PIN_10);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_9 | GPIO_PIN_10);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, GPIO_PIN_9 | GPIO_PIN_10);

    usart_deinit(USART0);
    usart_baudrate_set(USART0, s_baud_table[s_cfg.baud_index]);
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0, USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);
    nvic_irq_enable(USART0_IRQn, 2U);
    usart_interrupt_enable(USART0, USART_INT_RBNE);
}

/*!
    \brief      中断里写入接收环
    \param[in]  byte: 收到的字节
    \retval     none
*/
static void rs485_rx_push(uint8_t byte)
{
    uint8_t next = (uint8_t)((s_rx_w + 1U) & RS485_RX_MASK);

    if(next != s_rx_r) {
        s_rx[s_rx_w] = byte;
        s_rx_w = next;
    }
}

/*!
    \brief      等到标志置位，避免时钟异常时卡死
    \param[in]  flag: USART 标志
    \retval     0 等到了，-1 超时
*/
static int rs485_wait_flag(usart_flag_enum flag)
{
    uint32_t n = SystemCoreClock / 20U;

    while(RESET == usart_flag_get(USART0, flag)) {
        if(0U == n) {
            return -1;
        }
        n--;
    }
    return 0;
}

/*!
    \brief      拉高方向脚，发出一帧，再回到接收
    \param[in]  data: 整帧
    \param[in]  len: 字节数
    \retval     none
*/
static void rs485_send_raw(const uint8_t *data, uint8_t len)
{
    uint8_t i;

    gpio_bit_set(GPIOB, GPIO_PIN_5);
    rs485_delay_us(50U);
    for(i = 0U; i < len; i++) {
        if(0 != rs485_wait_flag(USART_FLAG_TBE)) {
            break;
        }
        usart_data_transmit(USART0, data[i]);
    }
    if(0 == rs485_wait_flag(USART_FLAG_TBE)) {
        usart_flag_clear(USART0, USART_FLAG_TC);
        (void)rs485_wait_flag(USART_FLAG_TC);
    }
    rs485_delay_us(50U);
    gpio_bit_reset(GPIOB, GPIO_PIN_5);
    s_last_tx_ms = systick_get_ms();
}

/*!
    \brief      组帧并发送。源地址固定为本机设备号
    \param[in]  dst: 目标
    \param[in]  cmd: 命令
    \param[in]  seq: 序号
    \param[in]  payload: 载荷，长度 0 时可以为空
    \param[in]  len: 载荷长度
    \retval     none
*/
static void rs485_send_frame(uint8_t dst, uint8_t cmd, uint8_t seq,
                             const uint8_t *payload, uint8_t len)
{
    uint8_t buf[RS485_FRAME_MAX];
    uint16_t crc;
    uint8_t i;

    if(len > RS485_PAYLOAD_MAX) {
        return;
    }
    buf[0] = RS485_SOF0;
    buf[1] = RS485_SOF1;
    buf[2] = dst;
    buf[3] = s_cfg.id;
    buf[4] = rs485_device_type();
    buf[5] = cmd;
    buf[6] = seq;
    buf[7] = len;
    for(i = 0U; i < len; i++) {
        buf[8U + i] = payload[i];
    }
    crc = rs485_crc16(&buf[2], (uint16_t)(6U + len));
    buf[8U + len] = (uint8_t)crc;
    buf[9U + len] = (uint8_t)(crc >> 8);
    rs485_send_raw(buf, (uint8_t)(10U + len));
}

/*!
    \brief      打包一帧角度，13 字节
    \param[out] p: 至少 13 字节
    \param[in]  sample: 解算结果
    \retval     none
*/
static void rs485_pack_sample(uint8_t *p, const hall_sample_t *sample)
{
    rs485_put_i32(&p[0], sample->angle_cdeg);
    rs485_put_i16(&p[4], sample->amplitude);
    rs485_put_u16(&p[6], sample->raw[0]);
    rs485_put_u16(&p[8], sample->raw[1]);
    rs485_put_u16(&p[10], sample->raw[2]);
    if(0U != rs485_is_line()) {
        p[12] = hall_line_flags(sample);
    } else {
        p[12] = hall_encoder_flags(sample);
    }
}

/*!
    \brief      读当前模式的一帧采样
    \param[out] sample: 角度或直线位置
    \retval     none
*/
static void rs485_sense_read(hall_sample_t *sample)
{
    if(0U != rs485_is_line()) {
        hall_line_read(sample);
    } else {
        hall_encoder_read(sample);
    }
}

/*!
    \brief      把当前模式的校准填进应答结构
    直线模式的 gain_q12[0] 是敏感宽度，单位 µm。
    \param[out] cal: 偏置或基线，以及增益或宽度
    \retval     none
*/
static void rs485_export_cal(hall_cal_t *cal)
{
    uint32_t i;

    memset(cal, 0, sizeof(*cal));
    if(0U != rs485_is_line()) {
        int16_t baseline[3];
        uint8_t valid;
        uint16_t sigma;
        int16_t zero;

        hall_line_get(baseline, &valid, &sigma, &zero);
        (void)zero;
        cal->valid = valid;
        cal->gain_q12[0] = sigma;
        for(i = 0U; i < 3U; i++) {
            cal->offset[i] = baseline[i];
        }
        return;
    }
    hall_encoder_cal_get(cal);
}

/*!
    \brief      执行一条已经通过校验的命令
    \param[in]  frame: 从 SOF 开始的整帧
    \retval     1 已经回答，0 没有回答
*/
static uint8_t rs485_handle(const uint8_t *frame)
{
    uint8_t dst = frame[2];
    uint8_t src = frame[3];
    uint8_t type = frame[4];
    uint8_t cmd = frame[5];
    uint8_t seq = frame[6];
    uint8_t len = frame[7];
    const uint8_t *in = &frame[8];
    uint8_t pay[24];
    uint8_t plen = 1U;
    uint8_t status = RS485_OK;
    uint8_t reply;
    uint8_t old_baud;
    uint8_t old_id;
    uint16_t period;
    uint16_t zero;
    hall_sample_t sample;
    hall_cal_t cal;
    uint8_t cal_rc;
    uint32_t i;

    if((RS485_TYPE_ANY != type) && (rs485_device_type() != type)) {
        return 0U;
    }
    if((dst != s_cfg.id) && (RS485_ADDR_BCAST != dst)) {
        return 0U;
    }
    if(0U != (cmd & RS485_CMD_RESPONSE)) {
        return 0U;
    }

    reply = (RS485_ADDR_BCAST == dst) ? 0U : 1U;
    if((0U == reply) &&
            ((RS485_CMD_GET_INFO == cmd) || (RS485_CMD_GET_DATA == cmd) ||
             (RS485_CMD_GET_CAL == cmd) || (RS485_CMD_SET_ID == cmd) ||
             (RS485_CMD_SET_BAUD == cmd) || (RS485_CMD_FACTORY == cmd))) {
        return 0U;
    }

    memset(pay, 0, sizeof(pay));
    switch(cmd) {
    case RS485_CMD_GET_INFO:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        rs485_sense_read(&sample);
        rs485_export_cal(&cal);
        pay[1] = rs485_device_type();
        pay[2] = s_cfg.id;
        pay[3] = s_cfg.mode;
        pay[4] = s_cfg.baud_index;
        rs485_put_u16(&pay[5], s_cfg.report_ms);
        rs485_put_u16(&pay[7], RS485_FW_VERSION);
        pay[9] = cal.valid;
        if(0U != rs485_is_line()) {
            pay[10] = hall_line_flags(&sample);
            rs485_put_i16(&pay[11], hall_line_zero_get());
        } else {
            pay[10] = hall_encoder_flags(&sample);
            rs485_put_u16(&pay[11], hall_encoder_zero_get());
        }
        plen = 13U;
        break;

    case RS485_CMD_SET_ID:
        if(1U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if((0U == in[0]) || (in[0] >= RS485_ADDR_BCAST)) {
            status = RS485_ERR_PARAM;
            break;
        }
        old_id = s_cfg.id;
        s_cfg.id = in[0];
        if(0 != rs485_cfg_save()) {
            s_cfg.id = old_id;
            status = RS485_ERR_FLASH;
            break;
        }
        pay[1] = s_cfg.id;
        plen = 2U;
        break;

    case RS485_CMD_SET_BAUD:
        if(1U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(in[0] >= RS485_BAUD_COUNT) {
            status = RS485_ERR_PARAM;
            break;
        }
        old_baud = s_cfg.baud_index;
        s_cfg.baud_index = in[0];
        if(0 != rs485_cfg_save()) {
            s_cfg.baud_index = old_baud;
            status = RS485_ERR_FLASH;
            break;
        }
        if(old_baud != s_cfg.baud_index) {
            s_reinit_uart = 1U;
        }
        pay[1] = s_cfg.baud_index;
        plen = 2U;
        break;

    case RS485_CMD_SET_MODE: {
        uint8_t old_mode = s_cfg.mode;
        uint16_t old_period = s_cfg.report_ms;

        if(3U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        period = rs485_get_u16(&in[1]);
        if((in[0] > RS485_MODE_REPORT) ||
                (period < RS485_REPORT_MIN_MS) || (period > RS485_REPORT_MAX_MS)) {
            status = RS485_ERR_PARAM;
            break;
        }
        s_cfg.mode = in[0];
        s_cfg.report_ms = period;
        if(0 != rs485_cfg_save()) {
            s_cfg.mode = old_mode;
            s_cfg.report_ms = old_period;
            status = RS485_ERR_FLASH;
            break;
        }
        pay[1] = s_cfg.mode;
        rs485_put_u16(&pay[2], s_cfg.report_ms);
        plen = 4U;
        break;
    }

    case RS485_CMD_SAVE:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0 != rs485_cfg_save()) {
            status = RS485_ERR_FLASH;
        }
        break;

    case RS485_CMD_FACTORY: {
        rs485_cfg_t old = s_cfg;

        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        hall_encoder_cal_clear();
        hall_encoder_zero_set(0U);
        hall_line_cal_clear();
        hall_line_zero_set(0);
        rs485_cfg_defaults();
        if(0 != rs485_cfg_save()) {
            s_cfg = old;
            rs485_cfg_push_hall();
            status = RS485_ERR_FLASH;
            break;
        }
        if(old.baud_index != s_cfg.baud_index) {
            s_reinit_uart = 1U;
        }
        pay[1] = s_cfg.id;
        pay[2] = s_cfg.baud_index;
        plen = 3U;
        break;
    }

    case RS485_CMD_GET_DATA:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        rs485_sense_read(&sample);
        rs485_pack_sample(&pay[1], &sample);
        plen = 14U;
        break;

    case RS485_CMD_SET_ZERO:
        if((0U != len) && (2U != len)) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0U != rs485_is_line()) {
            int16_t old_zero = hall_line_zero_get();

            if(2U == len) {
                hall_line_zero_set((int16_t)rs485_get_u16(in));
            } else {
                hall_line_zero_capture();
            }
            if(0 != rs485_cfg_save()) {
                hall_line_zero_set(old_zero);
                status = RS485_ERR_FLASH;
                break;
            }
            rs485_put_i16(&pay[1], hall_line_zero_get());
            plen = 3U;
            break;
        }
        zero = hall_encoder_zero_get();
        if(2U == len) {
            period = rs485_get_u16(in);
            if(period >= 36000U) {
                status = RS485_ERR_PARAM;
                break;
            }
            hall_encoder_zero_set(period);
        } else {
            hall_encoder_zero_capture();
        }
        if(0 != rs485_cfg_save()) {
            hall_encoder_zero_set(zero);
            status = RS485_ERR_FLASH;
            break;
        }
        rs485_put_u16(&pay[1], hall_encoder_zero_get());
        plen = 3U;
        break;

    case RS485_CMD_CLEAR_ZERO:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0U != rs485_is_line()) {
            hall_line_zero_set(0);
        } else {
            hall_encoder_zero_set(0U);
        }
        if(0 != rs485_cfg_save()) {
            status = RS485_ERR_FLASH;
            break;
        }
        break;

    case RS485_CMD_CAL_BEGIN:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0U != rs485_is_line()) {
            hall_line_cal_begin();
        } else {
            hall_encoder_cal_begin();
        }
        break;

    case RS485_CMD_CAL_END:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0U != rs485_is_line()) {
            cal_rc = hall_line_cal_finish();
        } else {
            cal_rc = hall_encoder_cal_finish();
        }
        if(HALL_CAL_IDLE == cal_rc) {
            status = RS485_ERR_STATE;
            break;
        }
        if(HALL_CAL_WEAK == cal_rc) {
            status = RS485_ERR_CAL;
            break;
        }
        if(0 != rs485_cfg_save()) {
            status = RS485_ERR_FLASH;
            break;
        }
        rs485_export_cal(&cal);
        for(i = 0U; i < 3U; i++) {
            rs485_put_i16(&pay[1U + (i * 2U)], cal.offset[i]);
            rs485_put_u16(&pay[7U + (i * 2U)], cal.gain_q12[i]);
        }
        plen = 13U;
        break;

    case RS485_CMD_CAL_CLEAR:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if(0U != rs485_is_line()) {
            hall_line_cal_clear();
        } else {
            hall_encoder_cal_clear();
        }
        if(0 != rs485_cfg_save()) {
            status = RS485_ERR_FLASH;
        }
        break;

    case RS485_CMD_GET_CAL:
        if(0U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        rs485_sense_read(&sample);
        rs485_export_cal(&cal);
        if(0U != rs485_is_line()) {
            pay[1] = hall_line_flags(&sample);
        } else {
            pay[1] = hall_encoder_flags(&sample);
        }
        for(i = 0U; i < 3U; i++) {
            rs485_put_i16(&pay[2U + (i * 2U)], cal.offset[i]);
            rs485_put_u16(&pay[8U + (i * 2U)], cal.gain_q12[i]);
        }
        plen = 14U;
        break;

    case RS485_CMD_SET_PROFILE: {
        rs485_cfg_t old;

        if(1U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        if((RS485_TYPE_HALL_ENCODER != in[0]) && (RS485_TYPE_HALL_POSITION != in[0])) {
            status = RS485_ERR_PARAM;
            break;
        }
        if(rs485_device_type() == in[0]) {
            pay[1] = rs485_device_type();
            plen = 2U;
            break;
        }
        old = s_cfg;
        rs485_enter_profile(in[0]);
        if(0 != rs485_cfg_save()) {
            s_cfg = old;
            rs485_cfg_push_hall();
            status = RS485_ERR_FLASH;
            break;
        }
        pay[1] = rs485_device_type();
        plen = 2U;
        break;
    }

    case RS485_CMD_SET_SIGMA: {
        uint16_t sigma;
        uint16_t old_sigma;

        if(0U == rs485_is_line()) {
            status = RS485_ERR_STATE;
            break;
        }
        if(2U != len) {
            status = RS485_ERR_LEN;
            break;
        }
        sigma = rs485_get_u16(in);
        if((sigma < HALL_LINE_SIGMA_MIN) || (sigma > HALL_LINE_SIGMA_MAX)) {
            status = RS485_ERR_PARAM;
            break;
        }
        old_sigma = s_cfg.gain_q12[0];
        s_cfg.gain_q12[0] = sigma;
        hall_line_sigma_set(sigma);
        if(0 != rs485_cfg_save()) {
            s_cfg.gain_q12[0] = old_sigma;
            hall_line_sigma_set(old_sigma);
            status = RS485_ERR_FLASH;
            break;
        }
        rs485_put_u16(&pay[1], s_cfg.gain_q12[0]);
        plen = 3U;
        break;
    }

    default:
        status = RS485_ERR_CMD;
        break;
    }

    pay[0] = status;
    if(0U == reply) {
        s_reinit_uart = 0U;
        return 0U;
    }
    rs485_send_frame(src, (uint8_t)(cmd | RS485_CMD_RESPONSE), seq, pay, plen);
    if(0U != s_reinit_uart) {
        s_reinit_uart = 0U;
        rs485_uart_baud(s_baud_table[s_cfg.baud_index]);
    }
    return 1U;
}

/*!
    \brief      收一个字节，凑齐一帧就执行
    \param[in]  byte: 新字节
    \retval     1 已回答
*/
static uint8_t rs485_asm_push(uint8_t byte)
{
    uint8_t plen;
    uint16_t crc;
    uint16_t got;

    if(0U == s_asm_len) {
        if(RS485_SOF0 != byte) {
            return 0U;
        }
        s_asm[0] = byte;
        s_asm_len = 1U;
        return 0U;
    }
    if(1U == s_asm_len) {
        if(RS485_SOF1 != byte) {
            s_asm_len = 0U;
            if(RS485_SOF0 == byte) {
                s_asm[0] = byte;
                s_asm_len = 1U;
            }
            return 0U;
        }
    }
    if(s_asm_len >= RS485_FRAME_MAX) {
        s_asm_len = 0U;
        return 0U;
    }
    s_asm[s_asm_len] = byte;
    s_asm_len++;
    if(s_asm_len < 8U) {
        return 0U;
    }
    plen = s_asm[7];
    if(plen > RS485_PAYLOAD_MAX) {
        s_asm_len = 0U;
        return 0U;
    }
    if(s_asm_len < (uint8_t)(10U + plen)) {
        return 0U;
    }
    crc = rs485_crc16(&s_asm[2], (uint16_t)(6U + plen));
    got = (uint16_t)s_asm[8U + plen] | ((uint16_t)s_asm[9U + plen] << 8);
    s_asm_len = 0U;
    if(crc != got) {
        return 0U;
    }
    return rs485_handle(s_asm);
}

/*!
    \brief      把接收环里的字节解成命令
    \retval     1 本轮回答过
*/
static uint8_t rs485_service_rx(void)
{
    uint8_t replied = 0U;
    uint32_t now = systick_get_ms();

    if((0U != s_asm_len) && ((now - s_asm_ms) > RS485_GAP_MS)) {
        s_asm_len = 0U;
    }
    while(s_rx_r != s_rx_w) {
        uint8_t byte = s_rx[s_rx_r];

        s_rx_r = (uint8_t)((s_rx_r + 1U) & RS485_RX_MASK);
        s_asm_ms = now;
        s_last_rx_ms = now;
        if(0U != rs485_asm_push(byte)) {
            replied = 1U;
        }
    }
    return replied;
}

/*!
    \brief      主动上报模式到点后发一帧角度
    \param[in]  replied: 本轮已经回答过查询
    \retval     none
*/
static void rs485_try_report(uint8_t replied)
{
    hall_sample_t sample;
    uint8_t payload[13];
    uint32_t now;

    if(0U != replied) {
        return;
    }
    if(RS485_MODE_REPORT != s_cfg.mode) {
        return;
    }
    if(0U != s_asm_len) {
        return;
    }
    now = systick_get_ms();
    if((now - s_last_rx_ms) < RS485_QUIET_MS) {
        return;
    }
    if((now - s_last_tx_ms) < RS485_QUIET_MS) {
        return;
    }
    if((now - s_report_ms) < s_cfg.report_ms) {
        return;
    }
    s_report_ms = now;
    rs485_sense_read(&sample);
    rs485_pack_sample(payload, &sample);
    rs485_send_frame(RS485_ADDR_HOST, RS485_CMD_REPORT, s_report_seq, payload, 13U);
    s_report_seq++;
}

void rs485_usart_isr(void)
{
    if(SET == usart_interrupt_flag_get(USART0, USART_INT_FLAG_RBNE)) {
        rs485_rx_push((uint8_t)usart_data_receive(USART0));
    }
    if(SET == usart_flag_get(USART0, USART_FLAG_ORERR)) {
        usart_flag_clear(USART0, USART_FLAG_ORERR);
    }
    if(SET == usart_flag_get(USART0, USART_FLAG_FERR)) {
        usart_flag_clear(USART0, USART_FLAG_FERR);
    }
    if(SET == usart_flag_get(USART0, USART_FLAG_NERR)) {
        usart_flag_clear(USART0, USART_FLAG_NERR);
    }
}

void rs485_proto_init(void)
{
    uint32_t now;

    rs485_cfg_load();
    rs485_uart_init();
    now = systick_get_ms();
    s_report_ms = now;
    s_last_tx_ms = now;
    s_last_rx_ms = now;
}

void rs485_proto_poll(void)
{
    uint8_t replied = rs485_service_rx();

    rs485_try_report(replied);
}

uint8_t rs485_proto_id(void)
{
    return s_cfg.id;
}

uint32_t rs485_proto_baud(void)
{
    return s_baud_table[s_cfg.baud_index];
}

uint8_t rs485_proto_mode(void)
{
    return s_cfg.mode;
}

uint8_t rs485_proto_type(void)
{
    return rs485_device_type();
}
