/*!
    \file    main.c
    \brief   三路霍尔角度通过 RTT 输出

    每 50 ms 打一行：
      adc <U1> <U2> <U4> ang <度.百分度> amp <峰值>
    amp 太小会额外带 weak，表示磁铁不在有效范围内，角度会乱跳。
    PB4 驱动状态灯，低电平点亮，约 500 ms 翻转一次。
    PB5 是 RS485 方向，保持低电平，收发器停在接收。
*/

#include "gd32e23x.h"
#include "systick.h"
#include "hall_encoder.h"
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
    \brief      应用入口
    \param[in]  none
    \param[out] none
    \retval     不返回
*/
int main(void)
{
    hall_sample_t sample;
    uint32_t beat = 0U;

    systick_config();
    SEGGER_RTT_Init();
    SEGGER_RTT_printf(0, "hall encoder U1 U2 U4\r\n");
    board_gpio_init();
    hall_encoder_init();

    while(1) {
        hall_encoder_read(&sample);
        hall_rtt_print(&sample);

        beat++;
        if(0U == (beat % 10U)) {
            gpio_bit_toggle(GPIOB, GPIO_PIN_4);
        }
        delay_1ms(50U);
    }
}
