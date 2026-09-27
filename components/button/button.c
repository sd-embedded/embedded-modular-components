/**
 * @file button.c
 * @brief Implementation of the portable button handler (see button.h).
 *
 * @author Szymon
 */

#include <stddef.h>
#include "button.h"

/* -------------------------------------------------------------------------- */
/* Static storage                                                             */
/* -------------------------------------------------------------------------- */

static button_t       button_pool[BUTTON_MAX_COUNT];
static button_event_t event_pool[BUTTON_EVENT_MAX_COUNT];

static uint8_t created_button_count = 0;
static uint8_t created_event_count  = 0;

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Validate that an event's timing parameters are consistent with its mode.
 * @param config Event configuration to check.
 * @return ::BUTTON_OK if valid, ::BUTTON_ERR_INVALID_PARAM otherwise.
 */
static button_status_t validate_event_config(const button_event_t *config)
{
	switch (config->mode)
	{
		case BUTTON_EVENT_ON_RELEASE:
			/* The accept window [min, max) must be non-empty. */
			if (config->max_hold_time_ms <= config->min_hold_time_ms)
			{
				return BUTTON_ERR_INVALID_PARAM;
			}
			break;

		case BUTTON_EVENT_ON_HOLD:
			break;

		case BUTTON_EVENT_REPEAT_WHILE_HELD:
			/* The period is used as a modulo divisor, so it must be non-zero. */
			if (config->repeat_period_ms == 0)
			{
				return BUTTON_ERR_INVALID_PARAM;
			}
			break;

		default:
			return BUTTON_ERR_INVALID_PARAM;
	}

	return BUTTON_OK;
}

/**
 * @brief Suppress every one-shot event on every button until all buttons go idle.
 *
 * ::BUTTON_EVENT_REPEAT_WHILE_HELD events are intentionally exempt.
 */
static void block_all_one_shot_events(void)
{
	for (uint8_t button_index = 0; button_index < created_button_count; button_index++)
	{
		button_event_t *event = button_pool[button_index].events;

		while (event != NULL)
		{
			if (event->mode != BUTTON_EVENT_REPEAT_WHILE_HELD)
			{
				event->is_blocked = true;
			}

			event = event->next;
		}
	}
}

/**
 * @brief Invoke an event's callback and apply its blocking policy.
 * @param event Event to fire (must be enabled and not blocked).
 */
static void fire_event(button_event_t *event)
{
	if (event->callback != NULL)
	{
		event->callback();
	}

	if (event->blocks_others)
	{
		block_all_one_shot_events();
	}
}

/**
 * @brief Evaluate hold-driven events (ON_HOLD, REPEAT_WHILE_HELD) for a held button.
 * @param button Button whose press is in progress.
 * @param event  Event to evaluate.
 */
static void evaluate_hold_event(const button_t *button, button_event_t *event)
{
	if (event->status != BUTTON_EVENT_ENABLED || event->is_blocked)
	{
		return;
	}

	if (event->mode == BUTTON_EVENT_ON_HOLD)
	{
		if (!event->has_fired && button->hold_time_ms >= event->min_hold_time_ms)
		{
			event->has_fired = true;
			fire_event(event);
		}
	}
	else if (event->mode == BUTTON_EVENT_REPEAT_WHILE_HELD)
	{
		if (button->hold_time_ms >= event->min_hold_time_ms)
		{
			uint32_t time_since_min_ms = button->hold_time_ms - event->min_hold_time_ms;

			if ((time_since_min_ms % event->repeat_period_ms) == 0)
			{
				fire_event(event);
			}
		}
	}
}

/**
 * @brief Evaluate release-driven events (ON_RELEASE) for a just-released button.
 * @param button Button whose release has been confirmed.
 * @param event  Event to evaluate.
 */
static void evaluate_release_event(const button_t *button, button_event_t *event)
{
	if (event->status != BUTTON_EVENT_ENABLED || event->is_blocked)
	{
		return;
	}

	if (event->mode == BUTTON_EVENT_ON_RELEASE)
	{
		if (button->hold_time_ms >= event->min_hold_time_ms &&
		    button->hold_time_ms <  event->max_hold_time_ms)
		{
			fire_event(event);
		}
	}
}

/**
 * @brief Handle a button reported as pressed: count hold time and run hold events.
 * @param button Target button.
 */
static void handle_press(button_t *button)
{
	button->debounce_elapsed_ms = 0;
	button->state = BUTTON_STATE_WAIT_RELEASE;

	if (button->hold_time_ms < UINT32_MAX)
	{
		button->hold_time_ms++;
	}

	for (button_event_t *event = button->events; event != NULL; event = event->next)
	{
		evaluate_hold_event(button, event);
	}
}

/**
 * @brief Handle the release-debounce state and, once confirmed, run release events.
 * @param button Target button.
 */
static void handle_release_debounce(button_t *button)
{
	/* Wait for the debounce window to elapse without a fresh press report. */
	if (button->debounce_elapsed_ms < button->debounce_time_ms)
	{
		button->debounce_elapsed_ms++;
		return;
	}

	button->state = BUTTON_STATE_IDLE;

	for (button_event_t *event = button->events; event != NULL; event = event->next)
	{
		evaluate_release_event(button, event);
	}

	/* Re-arm this button's one-shot latches for the next press. */
	for (button_event_t *event = button->events; event != NULL; event = event->next)
	{
		event->has_fired = false;
	}

	button->hold_time_ms = 0;
}

/**
 * @brief Advance a single button's state machine by one tick.
 * @param button Target button.
 */
static void process_button(button_t *button)
{
	switch (button->state)
	{
		case BUTTON_STATE_PRESSED:
			handle_press(button);
			break;

		case BUTTON_STATE_WAIT_RELEASE:
			handle_release_debounce(button);
			break;

		case BUTTON_STATE_IDLE:
		default:
			button->hold_time_ms = 0;
			break;
	}
}

/**
 * @brief Clear runtime blocking on all events once every button is idle.
 */
static void unblock_events_when_all_idle(void)
{
	for (uint8_t button_index = 0; button_index < created_button_count; button_index++)
	{
		if (button_pool[button_index].state != BUTTON_STATE_IDLE)
		{
			return;
		}
	}

	for (uint8_t button_index = 0; button_index < created_button_count; button_index++)
	{
		button_event_t *event = button_pool[button_index].events;

		while (event != NULL)
		{
			event->is_blocked = false;
			event = event->next;
		}
	}
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

button_status_t button_create(button_t **created_button)
{
	if (created_button == NULL)
	{
		return BUTTON_ERR_INVALID_PARAM;
	}

	if (created_button_count >= BUTTON_MAX_COUNT)
	{
		return BUTTON_ERR_NO_MEMORY;
	}

	button_t *button = &button_pool[created_button_count];
	created_button_count++;

	button->state               = BUTTON_STATE_IDLE;
	button->hold_time_ms        = 0;
	button->debounce_time_ms    = 0;
	button->debounce_elapsed_ms = 0;
	button->events              = NULL;

	*created_button = button;
	return BUTTON_OK;
}

void button_set_debounce(button_t *button, uint16_t debounce_time_ms)
{
	if (button != NULL)
	{
		button->debounce_time_ms = debounce_time_ms;
	}
}

button_status_t button_add_event(button_t *button, const button_event_t *config)
{
	if (button == NULL || config == NULL)
	{
		return BUTTON_ERR_NULL_CONFIG;
	}

	button_status_t validation_result = validate_event_config(config);
	if (validation_result != BUTTON_OK)
	{
		return validation_result;
	}

	if (created_event_count >= BUTTON_EVENT_MAX_COUNT)
	{
		return BUTTON_ERR_NO_MEMORY;
	}

	button_event_t *event = &event_pool[created_event_count];
	created_event_count++;

	/* Copy user configuration. */
	event->status           = config->status;
	event->mode             = config->mode;
	event->min_hold_time_ms = config->min_hold_time_ms;
	event->max_hold_time_ms = config->max_hold_time_ms;
	event->repeat_period_ms = config->repeat_period_ms;
	event->callback         = config->callback;
	event->blocks_others    = config->blocks_others;

	/* Initialise runtime fields. */
	event->is_blocked = false;
	event->has_fired  = false;
	event->next       = NULL;

	/* Append to the tail of the button's NULL-terminated event list. */
	if (button->events == NULL)
	{
		button->events = event;
	}
	else
	{
		button_event_t *tail_event = button->events;

		while (tail_event->next != NULL)
		{
			tail_event = tail_event->next;
		}

		tail_event->next = event;
	}

	return BUTTON_OK;
}

void button_report_pressed(button_t *button)
{
	if (button != NULL)
	{
		button->state = BUTTON_STATE_PRESSED;
	}
}

void button_reset_hold_time(button_t *button)
{
	if (button != NULL)
	{
		button->hold_time_ms = 0;
	}
}

void button_tick_1ms(void)
{
	unblock_events_when_all_idle();

	for (uint8_t button_index = 0; button_index < created_button_count; button_index++)
	{
		process_button(&button_pool[button_index]);
	}
}
