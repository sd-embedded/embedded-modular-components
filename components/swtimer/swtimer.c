/**
 * @file swtimer.c
 * @brief Implementation of the portable software timer middleware.
 *
 * See swtimer.h for the public API and usage notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "swtimer.h"

/*============================================================================
 * Named constants
 *==========================================================================*/

enum
{
	MILLISECONDS_PER_SECOND = 1000, /**< Borrow value when milliseconds roll under. */
	SECONDS_PER_MINUTE = 60,        /**< Borrow value when seconds roll under. */
};

/*============================================================================
 * Static storage
 *==========================================================================*/

/** @brief Static pool backing every registered timer. Zero-initialised. */
static swtimer_t timer_pool[SWTIMER_POOL_SIZE];

/** @brief Number of timers handed out so far; also the next free slot index. */
static uint16_t registered_timer_count = 0;

/*============================================================================
 * Internal helpers
 *==========================================================================*/

/**
 * @brief Reload a timer's remaining time from its initial value.
 * @param timer Timer to reload (must be valid).
 */
static void reload_countdown(swtimer_t *timer)
{
	timer->current = timer->initial;
}

/**
 * @brief Decrement a timer by one millisecond, borrowing across units.
 * @param timer Running timer to advance (must be valid).
 */
static void decrement_one_ms(swtimer_t *timer)
{
	if (timer->current.ms > 0)
	{
		timer->current.ms--;
	}
	else if (timer->current.sec > 0)
	{
		timer->current.sec--;
		timer->current.ms = MILLISECONDS_PER_SECOND - 1;
	}
	else if (timer->current.min > 0)
	{
		timer->current.min--;
		timer->current.sec = SECONDS_PER_MINUTE - 1;
		timer->current.ms = MILLISECONDS_PER_SECOND - 1;
	}
}

/**
 * @brief Test whether a duration is zero in every unit.
 * @param duration Duration to test (must be valid).
 * @return true if minutes, seconds and milliseconds are all zero.
 */
static bool is_duration_zero(const swtimer_time_t *duration)
{
	return (duration->ms == 0) && (duration->sec == 0) && (duration->min == 0);
}

/**
 * @brief Test whether a timer has counted down to zero.
 * @param timer Timer to test (must be valid).
 * @return true if no time remains.
 */
static bool has_reached_zero(const swtimer_t *timer)
{
	return is_duration_zero(&timer->current);
}

/**
 * @brief Handle a timer that has just expired: fire callback, then re-arm.
 *
 * Reloads the countdown in both modes so the timer is ready to run again. A
 * periodic timer keeps running; a one-shot timer is stopped.
 *
 * @param timer Expired timer (must be valid).
 */
static void handle_expiry(swtimer_t *timer)
{
	if (timer->callback != NULL)
	{
		timer->callback();
	}

	reload_countdown(timer);

	if (timer->mode == SWTIMER_MODE_ONE_SHOT)
	{
		timer->state = SWTIMER_STATE_STOPPED;
	}
}

/*============================================================================
 * Public API
 *==========================================================================*/

swtimer_err_t swtimer_register(const swtimer_config_t *config, swtimer_t **out_timer)
{
	if (config == NULL)
	{
		return SWTIMER_ERR_NULL_CONFIG;
	}

	if (out_timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	if (is_duration_zero(&config->period))
	{
		return SWTIMER_ERR_ZERO_PERIOD;
	}

	if (registered_timer_count >= SWTIMER_POOL_SIZE)
	{
		return SWTIMER_ERR_POOL_FULL;
	}

	swtimer_t *new_timer = &timer_pool[registered_timer_count];
	registered_timer_count++;

	new_timer->mode = config->mode;
	new_timer->state = SWTIMER_STATE_STOPPED;
	new_timer->initial = config->period;
	new_timer->current = config->period;
	new_timer->callback = config->callback;

	*out_timer = new_timer;
	return SWTIMER_OK;
}

swtimer_err_t swtimer_set_time(swtimer_t *timer, uint16_t ms, uint16_t sec, uint16_t min)
{
	if (timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	if (ms == 0 && sec == 0 && min == 0)
	{
		return SWTIMER_ERR_ZERO_PERIOD;
	}

	timer->initial.ms = ms;
	timer->initial.sec = sec;
	timer->initial.min = min;
	return SWTIMER_OK;
}

swtimer_err_t swtimer_start(swtimer_t *timer)
{
	if (timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	timer->state = SWTIMER_STATE_RUNNING;
	return SWTIMER_OK;
}

swtimer_err_t swtimer_stop(swtimer_t *timer)
{
	if (timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	timer->state = SWTIMER_STATE_STOPPED;
	reload_countdown(timer);
	return SWTIMER_OK;
}

swtimer_err_t swtimer_pause(swtimer_t *timer)
{
	if (timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	timer->state = SWTIMER_STATE_PAUSED;
	return SWTIMER_OK;
}

swtimer_err_t swtimer_reset(swtimer_t *timer)
{
	if (timer == NULL)
	{
		return SWTIMER_ERR_NULL_TIMER;
	}

	reload_countdown(timer);
	return SWTIMER_OK;
}

swtimer_err_t swtimer_reset_and_start(swtimer_t *timer)
{
	swtimer_err_t reset_result = swtimer_reset(timer);
	if (reset_result != SWTIMER_OK)
	{
		return reset_result;
	}

	return swtimer_start(timer);
}

swtimer_state_t swtimer_get_state(const swtimer_t *timer)
{
	if (timer == NULL)
	{
		return SWTIMER_STATE_STOPPED;
	}

	return timer->state;
}

void swtimer_tick_1ms(void)
{
	for (uint16_t slot_index = 0; slot_index < registered_timer_count; slot_index++)
	{
		swtimer_t *timer = &timer_pool[slot_index];

		if (timer->state != SWTIMER_STATE_RUNNING)
		{
			continue;
		}

		decrement_one_ms(timer);

		if (has_reached_zero(timer))
		{
			handle_expiry(timer);
		}
	}
}

void swtimer_pool_clear(void)
{
	memset(timer_pool, 0, sizeof(timer_pool));
	registered_timer_count = 0;
}
