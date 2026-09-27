/**
 * @file basic_usage.c
 * @brief Minimal, self-contained example of the led_anim module.
 *
 * The example builds and runs on a host PC (no MCU required):
 *
 *     gcc -std=c99 -Wall -Wextra basic_usage.c led_anim.c -o basic_usage
 *     ./basic_usage
 *
 * On a real target:
 *  - the pwm_channel_* callbacks would write a timer compare register instead
 *    of storing into a variable, and
 *  - led_anim_tick_1ms() would be called from a 1 ms SysTick / timer interrupt
 *    instead of the simulated loop in run_milliseconds().
 */

#include <stdio.h>

#include "led_anim.h"

#define DEMO_LED_COUNT 2u

/* Two software "PWM channels" standing in for real hardware outputs. */
static led_anim_level_t g_channel_level[DEMO_LED_COUNT];

static void pwm_channel_0(led_anim_level_t level)
{
	g_channel_level[0] = level;
}

static void pwm_channel_1(led_anim_level_t level)
{
	g_channel_level[1] = level;
}

/* Pretend that @p duration_ms milliseconds pass: drive the module each ms and
 * print the resulting output levels so the animation is visible on stdout. */
static void run_milliseconds(uint16_t duration_ms)
{
	for (uint16_t elapsed_ms = 0u; elapsed_ms < duration_ms; elapsed_ms++)
	{
		led_anim_tick_1ms();
		printf("  ch0 = %4u   ch1 = %4u\n",
		       (unsigned)g_channel_level[0],
		       (unsigned)g_channel_level[1]);
	}
}

int main(void)
{
	static const led_anim_config_t config =
	{
		.mode          = LED_ANIM_MODE_BLINK,
		.num_leds      = DEMO_LED_COUNT,
		.mask          = 0x03u, /* enable LED 0 and LED 1 */
		.level_max     = 100u,
		.level_min     = 0u,
		.blink_dead_ms = 3u,    /* pause 3 ms at each blink extreme */
		.ramp_step     = 20,    /* +20 levels per millisecond */
	};

	led_anim_obj_t   *leds;
	led_anim_status_t status;

	status = led_anim_create(&config, &leds);
	if (status != LED_ANIM_OK)
	{
		printf("led_anim_create failed: %d\n", (int)status);
		return 1;
	}

	led_anim_attach_pwm(leds, 0u, pwm_channel_0);
	led_anim_attach_pwm(leds, 1u, pwm_channel_1);

	printf("BLINK: ramp up, dwell at top, ramp back down\n");
	run_milliseconds(12u);

	printf("Switch to steady ON\n");
	led_anim_set_mode(leds, LED_ANIM_MODE_ON);
	run_milliseconds(6u);

	printf("Switch OFF\n");
	led_anim_set_mode(leds, LED_ANIM_MODE_OFF);
	run_milliseconds(6u);

	return 0;
}
