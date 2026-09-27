/**
 * @file led_anim.c
 * @brief Implementation of the portable LED animation middleware.
 *
 * Concurrency model
 * -----------------
 * ::led_anim_tick_1ms is expected to run from a 1 ms periodic context (e.g. a
 * timer ISR) while every other function runs from main/thread context. The
 * shared state is protected with a cooperative "busy" flag: a writer raises it
 * for the whole operation, and the tick skips the current cycle if it finds the
 * flag raised. This keeps the module HAL-independent (no interrupt-masking
 * intrinsics). The only side effect is that a tick coinciding with a writer may
 * be dropped, which is harmless for LED timing.
 */

#include <stdbool.h>
#include <stddef.h>

#include "led_anim.h"

/* ------------------------------------------------------------------------- */
/* Private types                                                             */
/* ------------------------------------------------------------------------- */

/** @brief Runtime state of a single LED (kept private to this module). */
typedef struct
{
	led_anim_level_t  value_actual;  /**< Current brightness, converges to target. */
	led_anim_level_t  value_target;  /**< Brightness the ramp drives toward. */
	uint16_t          ramp_tick_ms;  /**< Countdown used only when ramp_step < 0. */
	uint16_t          blink_dead_ms; /**< Remaining dwell time at a blink extreme. */
	led_anim_pwm_fn_t pwm_fn;        /**< Output callback (NULL = unused). */
} led_anim_led_t;

/** @brief Concrete definition of the opaque ::led_anim_obj_t handle. */
struct led_anim_obj
{
	led_anim_mode_t  mode;
	uint8_t          num_leds;
	uint32_t         mask;
	led_anim_level_t level_max;
	led_anim_level_t level_min;
	uint16_t         blink_dead_ms;
	int16_t          ramp_step;
	led_anim_led_t  *leds;           /**< Slice of led_pool, length num_leds. */
};

/* ------------------------------------------------------------------------- */
/* Named constants                                                           */
/* ------------------------------------------------------------------------- */

/** @brief Brightness level that turns an LED fully off. */
static const led_anim_level_t LEVEL_OFF = 0u;

/** @brief Cooperative-lock state: no writer is touching the shared state. */
static const uint8_t LOCK_FREE = 0u;

/** @brief Cooperative-lock state: a writer owns the shared state; tick backs off. */
static const uint8_t LOCK_HELD = 1u;

/* ------------------------------------------------------------------------- */
/* Static storage                                                            */
/* ------------------------------------------------------------------------- */

static struct led_anim_obj obj_pool[LED_ANIM_OBJECT_MAX];
static led_anim_led_t      led_pool[LED_ANIM_LED_POOL_SIZE];

static uint8_t  obj_pool_next; /**< Next free object slot. */
static uint16_t led_pool_next; /**< Next free LED slot (uint16_t avoids >255 overflow). */

static volatile uint8_t busy;  /**< Cooperative lock between writers and the tick. */

/* ------------------------------------------------------------------------- */
/* Critical section (cooperative)                                            */
/* ------------------------------------------------------------------------- */

static inline void enter_critical(void)
{
	busy = LOCK_HELD;
}

static inline void exit_critical(void)
{
	busy = LOCK_FREE;
}

/* ------------------------------------------------------------------------- */
/* Predicates                                                                */
/* ------------------------------------------------------------------------- */

/** @brief Tell whether the LED at @p led_index is selected by the enable mask. */
static bool is_led_enabled(const struct led_anim_obj *obj, uint8_t led_index)
{
	return (obj->mask & ((uint32_t)1u << led_index)) != 0u;
}

/** @brief Tell whether an LED still has dwell time left at a blink extreme. */
static bool has_dwell_remaining(const led_anim_led_t *led)
{
	return led->blink_dead_ms > 0u;
}

/* ------------------------------------------------------------------------- */
/* Tick helpers                                                              */
/* ------------------------------------------------------------------------- */

/** @brief Drive the blink ("breathing") target for one enabled LED. */
static void blink_update_target(const struct led_anim_obj *obj, led_anim_led_t *led)
{
	if (led->value_actual > obj->level_max)
	{
		led->value_target = obj->level_min;
	}
	if (led->value_actual < obj->level_min)
	{
		led->value_target = obj->level_max;
	}

	if (led->value_actual == obj->level_max)
	{
		if (has_dwell_remaining(led))
		{
			led->blink_dead_ms--;
		}
		else
		{
			led->value_target = obj->level_min;
			led->blink_dead_ms = obj->blink_dead_ms;
		}
	}
	else if (led->value_actual == obj->level_min)
	{
		if (has_dwell_remaining(led))
		{
			led->blink_dead_ms--;
		}
		else
		{
			led->value_target = obj->level_max;
			led->blink_dead_ms = obj->blink_dead_ms;
		}
	}
	else if (led->value_target != obj->level_min && led->value_target != obj->level_max)
	{
		led->value_target = obj->level_min;
	}
}

/** @brief Pick the LED's target brightness based on the group's mode. */
static void compute_target(const struct led_anim_obj *obj, led_anim_led_t *led, uint8_t led_index)
{
	switch (obj->mode)
	{
	case LED_ANIM_MODE_OFF:
		led->value_target = LEVEL_OFF;
		break;

	case LED_ANIM_MODE_ON:
		led->value_target = is_led_enabled(obj, led_index) ? obj->level_max : LEVEL_OFF;
		break;

	case LED_ANIM_MODE_BLINK:
		if (is_led_enabled(obj, led_index))
		{
			blink_update_target(obj, led);
		}
		else
		{
			led->value_target = LEVEL_OFF;
		}
		break;

	default:
		break;
	}
}

/** @brief Move value_actual toward value_target by more than one level per ms. */
static void ramp_by_steps_per_ms(led_anim_led_t *led, uint16_t increment)
{
	if (led->value_actual < led->value_target)
	{
		if ((uint16_t)(led->value_target - led->value_actual) < increment)
		{
			led->value_actual = led->value_target;
		}
		else
		{
			led->value_actual += increment;
		}
	}
	else if (led->value_actual > led->value_target)
	{
		if ((uint16_t)(led->value_actual - led->value_target) < increment)
		{
			led->value_actual = led->value_target;
		}
		else
		{
			led->value_actual -= increment;
		}
	}
}

/** @brief Move value_actual toward value_target by one level every period_ms. */
static void ramp_by_ms_per_step(led_anim_led_t *led, uint16_t period_ms)
{
	if (led->ramp_tick_ms > 0u)
	{
		led->ramp_tick_ms--;
		if (led->ramp_tick_ms == 0u)
		{
			led->ramp_tick_ms = period_ms;
			if (led->value_actual < led->value_target)
			{
				led->value_actual++;
			}
			else if (led->value_actual > led->value_target)
			{
				led->value_actual--;
			}
		}
	}
	else
	{
		led->ramp_tick_ms = period_ms;
	}
}

/** @brief Advance value_actual one tick toward value_target per the ramp speed. */
static void apply_ramp(const struct led_anim_obj *obj, led_anim_led_t *led)
{
	int16_t step = obj->ramp_step;

	if (step > 0)
	{
		ramp_by_steps_per_ms(led, (uint16_t)step);
	}
	else if (step < 0)
	{
		ramp_by_ms_per_step(led, (uint16_t)(-(int32_t)step));
	}
	else
	{
		led->value_actual = led->value_target; /* step == 0: jump instantly */
	}
}

/** @brief Update one LED for this tick and push its level to the hardware. */
static void update_led(const struct led_anim_obj *obj, led_anim_led_t *led, uint8_t led_index)
{
	compute_target(obj, led, led_index);
	apply_ramp(obj, led);

	if (led->pwm_fn != NULL)
	{
		led->pwm_fn(led->value_actual);
	}
}

/** @brief Update every LED of one group. */
static void update_object(struct led_anim_obj *obj)
{
	for (uint8_t led_index = 0u; led_index < obj->num_leds; led_index++)
	{
		update_led(obj, &obj->leds[led_index], led_index);
	}
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

led_anim_status_t led_anim_create(const led_anim_config_t *config, led_anim_obj_t **out_obj)
{
	struct led_anim_obj *obj;

	if (config == NULL || out_obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	if (config->num_leds == 0u || config->num_leds > LED_ANIM_MAX_LEDS_PER_GROUP)
	{
		return LED_ANIM_ERR_RANGE;
	}

	if (obj_pool_next >= LED_ANIM_OBJECT_MAX)
	{
		return LED_ANIM_ERR_NO_MEMORY;
	}

	if ((uint16_t)(led_pool_next + config->num_leds) > LED_ANIM_LED_POOL_SIZE)
	{
		return LED_ANIM_ERR_NO_MEMORY;
	}

	/* All checks passed: commit the pool reservations. */
	obj = &obj_pool[obj_pool_next++];
	obj->leds = &led_pool[led_pool_next];
	led_pool_next = (uint16_t)(led_pool_next + config->num_leds);

	obj->mode          = config->mode;
	obj->num_leds      = config->num_leds;
	obj->mask          = config->mask;
	obj->level_max     = config->level_max;
	obj->level_min     = config->level_min;
	obj->blink_dead_ms = config->blink_dead_ms;
	obj->ramp_step     = config->ramp_step;

	for (uint8_t led_index = 0u; led_index < obj->num_leds; led_index++)
	{
		obj->leds[led_index].value_actual  = LEVEL_OFF;
		obj->leds[led_index].value_target  = LEVEL_OFF;
		obj->leds[led_index].ramp_tick_ms  = 0u;
		obj->leds[led_index].blink_dead_ms = 0u;
		obj->leds[led_index].pwm_fn        = NULL;
	}

	*out_obj = obj;
	return LED_ANIM_OK;
}

led_anim_status_t led_anim_attach_pwm(led_anim_obj_t *obj, uint8_t led_index, led_anim_pwm_fn_t pwm_fn)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	if (led_index >= obj->num_leds)
	{
		return LED_ANIM_ERR_INDEX;
	}

	enter_critical();
	obj->leds[led_index].pwm_fn = pwm_fn;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_mode(led_anim_obj_t *obj, led_anim_mode_t mode)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->mode = mode;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_mask(led_anim_obj_t *obj, uint32_t mask)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->mask = mask;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_level_max(led_anim_obj_t *obj, led_anim_level_t level)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->level_max = level;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_level_min(led_anim_obj_t *obj, led_anim_level_t level)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->level_min = level;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_ramp_step(led_anim_obj_t *obj, int16_t ramp_step)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->ramp_step = ramp_step;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_blink_dead_time(led_anim_obj_t *obj, uint16_t dead_time_ms)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	obj->blink_dead_ms = dead_time_ms;
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_active_value(led_anim_obj_t *obj, led_anim_level_t value)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	if (value < obj->level_min || value > obj->level_max)
	{
		return LED_ANIM_ERR_RANGE;
	}

	enter_critical();
	for (uint8_t led_index = 0u; led_index < obj->num_leds; led_index++)
	{
		if (is_led_enabled(obj, led_index))
		{
			obj->leds[led_index].value_actual  = value;
			obj->leds[led_index].blink_dead_ms = obj->blink_dead_ms;
			obj->leds[led_index].value_target  = obj->level_max;
		}
	}
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_active_target(led_anim_obj_t *obj, led_anim_level_t value)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	if (value < obj->level_min || value > obj->level_max)
	{
		return LED_ANIM_ERR_RANGE;
	}

	enter_critical();
	for (uint8_t led_index = 0u; led_index < obj->num_leds; led_index++)
	{
		if (is_led_enabled(obj, led_index))
		{
			obj->leds[led_index].value_target = value;
		}
	}
	exit_critical();

	return LED_ANIM_OK;
}

led_anim_status_t led_anim_set_inactive_dead_time(led_anim_obj_t *obj, uint16_t dead_time_ms)
{
	if (obj == NULL)
	{
		return LED_ANIM_ERR_NULL;
	}

	enter_critical();
	for (uint8_t led_index = 0u; led_index < obj->num_leds; led_index++)
	{
		if (!is_led_enabled(obj, led_index))
		{
			obj->leds[led_index].blink_dead_ms = dead_time_ms;
		}
	}
	exit_critical();

	return LED_ANIM_OK;
}

void led_anim_tick_1ms(void)
{
	if (busy == LOCK_HELD)
	{
		return;
	}

	for (uint8_t object_index = 0u; object_index < obj_pool_next; object_index++)
	{
		update_object(&obj_pool[object_index]);
	}
}

void led_anim_reset(void)
{
	enter_critical();
	obj_pool_next = 0u;
	led_pool_next = 0u;
	exit_critical();
}
