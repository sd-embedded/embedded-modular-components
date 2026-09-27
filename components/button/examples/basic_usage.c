/**
 * @file basic_usage.c
 * @brief Minimal usage example for the button module.
 *
 * Demonstrates a single physical button carrying three coexisting events:
 *   - a short press (fires once on release within a time window),
 *   - a long hold (fires once after a hold threshold, and cancels the short
 *     press for that gesture via @c blocks_others),
 *   - an auto-repeat (fires periodically while the button stays held).
 *
 * The example is platform-independent: the physical pin read is stubbed by
 * ::board_button_is_pressed, which a real application replaces with a GPIO read.
 *
 * Wiring into an application:
 *   - call ::app_buttons_init once at startup, and
 *   - call ::app_buttons_service every millisecond (e.g. from a 1 ms timer or
 *     SysTick interrupt).
 *
 * @author Szymon
 */

#include <stdbool.h>
#include <stdio.h>

#include "button.h"

/* -------------------------------------------------------------------------- */
/* Timing configuration (named constants instead of magic numbers)            */
/* -------------------------------------------------------------------------- */

#define DEBOUNCE_TIME_MS        20u  /**< Release debounce window.               */
#define SHORT_PRESS_MIN_MS      20u  /**< Shortest press counted as a tap.       */
#define SHORT_PRESS_MAX_MS     500u  /**< Longest press still counted as a tap.  */
#define LONG_HOLD_MIN_MS      1000u  /**< Hold threshold for the long-hold event.*/
#define REPEAT_START_MS      1000u   /**< Hold time after which auto-repeat runs. */
#define REPEAT_PERIOD_MS      200u   /**< Auto-repeat interval while held.       */

/* -------------------------------------------------------------------------- */
/* Application-provided I/O (replace with a real GPIO read)                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Return true while the physical button is pressed.
 *
 * Stub for the example. In a real project this reads the button's GPIO pin,
 * e.g. @c return HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN) == GPIO_PIN_RESET;
 */
static bool board_button_is_pressed(void)
{
	return false;
}

/* -------------------------------------------------------------------------- */
/* Event callbacks                                                            */
/* -------------------------------------------------------------------------- */

static void on_short_press(void)
{
	printf("short press: toggle output\n");
}

static void on_long_hold(void)
{
	printf("long hold: enter configuration mode\n");
}

static void on_repeat_while_held(void)
{
	printf("repeat: increment value\n");
}

/* -------------------------------------------------------------------------- */
/* Setup and service                                                          */
/* -------------------------------------------------------------------------- */

static button_t *user_button;

/**
 * @brief Create the button and attach its three events. Call once at startup.
 */
static void app_buttons_init(void)
{
	if (button_create(&user_button) != BUTTON_OK)
	{
		return;
	}

	button_set_debounce(user_button, DEBOUNCE_TIME_MS);

	/* Short press: released between SHORT_PRESS_MIN_MS and SHORT_PRESS_MAX_MS. */
	const button_event_t short_press =
	{
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_ON_RELEASE,
		.min_hold_time_ms = SHORT_PRESS_MIN_MS,
		.max_hold_time_ms = SHORT_PRESS_MAX_MS,
		.callback         = on_short_press,
	};
	button_add_event(user_button, &short_press);

	/* Long hold: fires once at LONG_HOLD_MIN_MS and cancels the short press. */
	const button_event_t long_hold =
	{
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_ON_HOLD,
		.min_hold_time_ms = LONG_HOLD_MIN_MS,
		.blocks_others    = true,
		.callback         = on_long_hold,
	};
	button_add_event(user_button, &long_hold);

	/* Auto-repeat: every REPEAT_PERIOD_MS once held for REPEAT_START_MS. */
	const button_event_t repeat_while_held =
	{
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_REPEAT_WHILE_HELD,
		.min_hold_time_ms = REPEAT_START_MS,
		.repeat_period_ms = REPEAT_PERIOD_MS,
		.callback         = on_repeat_while_held,
	};
	button_add_event(user_button, &repeat_while_held);
}

/**
 * @brief Sample the pin and advance the module. Call once per millisecond.
 */
static void app_buttons_service(void)
{
	if (board_button_is_pressed())
	{
		button_report_pressed(user_button);
	}

	button_tick_1ms();
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

int main(void)
{
	app_buttons_init();

	/* A real firmware loop never returns; here a bounded loop keeps the
	 * example self-contained. Each iteration stands in for one 1 ms tick. */
	for (uint32_t elapsed_ms = 0; elapsed_ms < 3000u; elapsed_ms++)
	{
		app_buttons_service();
	}

	return 0;
}
