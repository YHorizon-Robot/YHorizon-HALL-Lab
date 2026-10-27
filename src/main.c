/*!
    \file    main.c
    \brief   三路霍尔角度或直线位置，RTT 调试，RS485 对外通信

    角度模式每 50 ms 打一行：
      adc <U1> <U2> <U4> ang <度.百分度> amp <峰值>
    直线模式打：
      adc <U1> <U2> <U4> pos <毫米.百分毫米> mm amp <峰值>
    amp 太小会额外带 weak。直线位置超出阵列两端时带 out。
    PB4 驱动状态灯，低电平点亮，约 500 ms 翻转一次。
    PB5 是 RS485 方向，上电先拉低，随后由协议模块控制。
*/

#include "gd32e23x.h"
#include "systick.h"
#include "hall_encoder.h"
#include "hall_line.h"
#include "rs485_proto.h"
#include "SEGGER_RTT.h"

/*!
    \brief      状态灯和 RS485 方向脚
    \param[in]  none
    \param[out] none
    \retval     none
*/
static void board_gpio_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOB);

    gpio_mode_set(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO_PIN_4 | GPIO_PIN_5);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, GPIO_PIN_4 | GPIO_PIN_5);
    gpio_bit_set(GPIOB, GPIO_PIN_4);
    /*! 收发器默认接收。rs485_proto_init 会再次配置这只脚。 */
    gpio_bit_reset(GPIOB, GPIO_PIN_5);
}

/*!
    \brief      把一帧霍尔结果打到 RTT
    \param[in]  sample: 原码和角度
    \param[out] none
    \retval     none
*/
static void hall_rtt_print(const hall_sample_t *sample)
{
    int32_t whole = sample->angle_cdeg / 100;
    int32_t frac = sample->angle_cdeg % 100;

    if(sample->amplitude < HALL_AMP_WEAK) {
        SEGGER_RTT_printf(0, "adc %u %u %u ang %d.%02d amp %d weak\r\n",
                          (unsigned)sample->raw[0], (unsigned)sample->raw[1], (unsigned)sample->raw[2],
                          (int)whole, (int)frac, (int)sample->amplitude);
    } else {
        SEGGER_RTT_printf(0, "adc %u %u %u ang %d.%02d amp %d\r\n",
                          (unsigned)sample->raw[0], (unsigned)sample->raw[1], (unsigned)sample->raw[2],
                          (int)whole, (int)frac, (int)sample->amplitude);
    }
}

/*!
    \brief      把直线位置打到 RTT，负数的小数部分取绝对值
    \param[in]  sample: 位置在 angle_cdeg，单位 0.01 mm
    \retval     none
*/
static void hall_rtt_print_pos(const hall_sample_t *sample)
{
    int32_t value = sample->angle_cdeg;
    int32_t whole;
    int32_t frac;
    uint8_t flags = hall_line_flags(sample);
    const char *sign = "";
    const char *note = "";

    if(value < 0) {
        sign = "-";
        value = -value;
    }
    whole = value / 100;
    frac = value % 100;
    if((0U != (flags & HALL_FLAG_WEAK)) && (0U != (flags & HALL_FLAG_OUT))) {
        note = " weak out";
    } else if(0U != (flags & HALL_FLAG_WEAK)) {
        note = " weak";
    } else if(0U != (flags & HALL_FLAG_OUT)) {
        note = " out";
    }
    SEGGER_RTT_printf(0, "adc %u %u %u pos %s%d.%02d mm amp %d%s\r\n",
                      (unsigned)sample->raw[0], (unsigned)sample->raw[1], (unsigned)sample->raw[2],
                      sign, (int)whole, (int)frac, (int)sample->amplitude, note);
}

/*!
    \brief      应用入口
    \param[in]  none
    \param[out] none
    \retval     不返回
*/
int main(void)
{
    hall_sample_t sample;
    uint32_t rtt_ms = 0U;
    uint32_t led_ms = 0U;

    systick_config();
    SEGGER_RTT_Init();
    SEGGER_RTT_printf(0, "hall U1 U2 U4\r\n");
    board_gpio_init();
    hall_encoder_init();
    rs485_proto_init();
    SEGGER_RTT_printf(0, "rs485 id %u baud %u mode %u type %u\r\n",
                      (unsigned)rs485_proto_id(),
                      (unsigned)rs485_proto_baud(),
                      (unsigned)rs485_proto_mode(),
                      (unsigned)rs485_proto_type());

    while(1) {
        uint32_t now = systick_get_ms();
        uint8_t linear = (RS485_TYPE_HALL_POSITION == rs485_proto_type()) ? 1U : 0U;

        /*! 主循环持续采样。角度模式记峰峰值，直线模式记零磁场平均。 */
        if(0U != linear) {
            hall_line_read(&sample);
        } else {
            hall_encoder_read(&sample);
        }
        rs485_proto_poll();

        if((now - rtt_ms) >= 50U) {
            rtt_ms = now;
            if(0U != linear) {
                hall_rtt_print_pos(&sample);
            } else {
                hall_rtt_print(&sample);
            }
        }
        if((now - led_ms) >= 500U) {
            led_ms = now;
            gpio_bit_toggle(GPIOB, GPIO_PIN_4);
        }
    }
}
