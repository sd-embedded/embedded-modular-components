/**
 * @file led_anim.h
 * @brief Portable, HAL-independent LED animation middleware.
 *
 * The module manages groups of LEDs and drives their brightness over time:
 * static on/off, and a smooth blink ("breathing") between a minimum and a
 * maximum level. Brightness changes are applied through ramps and pushed to the
 * hardware via a user-supplied PWM callback, one per LED.
 *
 * Design notes:
 *  - No dynamic allocation. Objects and per-LED state live in static pools whose
 *    sizes are configurable via the macros below (overridable with -D).
 *  - The object handle (::led_anim_obj_t) is opaque: all access goes through the
 *    public functions, never through struct members.
 *  - ::led_anim_tick_1ms is meant to be called from a periodic 1 ms context
 *    (e.g. SysTick / timer ISR). All other functions are meant for thread/main
 *    context. The implementation guards the shared state internally.
 */

#ifndef LED_ANIM_H_
#define LED_ANIM_H_

#include <stdint.h>

/**
 * @name Compile-time configuration
 * Override any of these from the build system (e.g. -DLED_ANIM_OBJECT_MAX=4).
 * @{
 */

/** @brief Maximum number of LED groups (objects) held in the static pool. */
#ifndef LED_ANIM_OBJECT_MAX
#define LED_ANIM_OBJECT_MAX 11u
#endif

/** @brief Total number of per-LED slots shared by all groups. */
#ifndef LED_ANIM_LED_POOL_SIZE
#define LED_ANIM_LED_POOL_SIZE 11u
#endif

/** @brief Maximum LEDs per group, bounded by the 32-bit enable mask width. */
#define LED_ANIM_MAX_LEDS_PER_GROUP 32u

/** @} */

/**
 * @brief Brightness level.
 *
 * 0 means fully off. The upper bound and physical meaning are defined by the
 * PWM backend the user wires up via ::led_anim_attach_pwm (e.g. a timer ARR).
 */
typedef uint16_t led_anim_level_t;

/**
 * @brief Status / error codes returned by the public API.
 */
typedef enum
{
	LED_ANIM_OK = 0,          /**< Operation succeeded. */
	LED_ANIM_ERR_NULL,        /**< A required pointer argument was NULL. */
	LED_ANIM_ERR_NO_MEMORY,   /**< Object pool or LED pool exhausted. */
	LED_ANIM_ERR_RANGE,       /**< A value or count was outside the allowed range. */
	LED_ANIM_ERR_INDEX,       /**< LED index was out of range for the group. */
} led_anim_status_t;

/**
 * @brief Animation mode of an LED group.
 */
typedef enum
{
	LED_ANIM_MODE_OFF = 0,    /**< All LEDs driven to 0. */
	LED_ANIM_MODE_ON,         /**< Enabled LEDs driven to level_max, others to 0. */
	LED_ANIM_MODE_BLINK,      /**< Enabled LEDs ramp smoothly between level_min and level_max. */
} led_anim_mode_t;

/**
 * @brief Low-level PWM output callback for a single LED.
 *
 * @param level Brightness level to apply (0 = off).
 */
typedef void (*led_anim_pwm_fn_t)(led_anim_level_t level);

/**
 * @brief Opaque handle to an LED group.
 *
 * The concrete definition lives in led_anim.c. Use the public functions to
 * interact with it; do not dereference it.
 */
typedef struct led_anim_obj led_anim_obj_t;

/**
 * @brief Configuration passed to ::led_anim_create.
 */
typedef struct
{
	led_anim_mode_t  mode;          /**< Initial animation mode. */
	uint8_t          num_leds;      /**< LEDs in the group (1..::LED_ANIM_MAX_LEDS_PER_GROUP). */
	uint32_t         mask;          /**< Enable mask; bit i selects LED i. */
	led_anim_level_t level_max;     /**< Max level for ON and upper bound for BLINK. */
	led_anim_level_t level_min;     /**< Lower bound for BLINK. */
	uint16_t         blink_dead_ms; /**< Dwell time (ms) at a blink extreme before reversing. */
	int16_t          ramp_step;     /**< Ramp speed: >0 = steps per ms, <0 = 1 step per |n| ms, 0 = instant. */
} led_anim_config_t;

/**
 * @brief Create a new LED group from a configuration.
 *
 * Reserves one object slot and @p num_leds slots from the shared LED pool, then
 * copies the configuration. All per-LED PWM callbacks start as NULL and must be
 * wired up with ::led_anim_attach_pwm.
 *
 * @param[in]  config   Configuration to apply. Must not be NULL.
 * @param[out] out_obj  Receives the handle to the created group on success.
 *                      Must not be NULL.
 * @return ::LED_ANIM_OK on success,
 *         ::LED_ANIM_ERR_NULL if @p config or @p out_obj is NULL,
 *         ::LED_ANIM_ERR_RANGE if @p num_leds is 0 or exceeds
 *         ::LED_ANIM_MAX_LEDS_PER_GROUP,
 *         ::LED_ANIM_ERR_NO_MEMORY if the object or LED pool is exhausted.
 */
led_anim_status_t led_anim_create(const led_anim_config_t *config, led_anim_obj_t **out_obj);

/**
 * @brief Attach the PWM output callback for one LED of a group.
 *
 * @param obj        Target group. Must not be NULL.
 * @param led_index  LED index within the group (0-based).
 * @param pwm_fn     Callback invoked each tick with the LED's current level.
 *                   May be NULL to detach.
 * @return ::LED_ANIM_OK on success,
 *         ::LED_ANIM_ERR_NULL if @p obj is NULL,
 *         ::LED_ANIM_ERR_INDEX if @p led_index is out of range.
 */
led_anim_status_t led_anim_attach_pwm(led_anim_obj_t *obj, uint8_t led_index, led_anim_pwm_fn_t pwm_fn);

/**
 * @brief Set the animation mode of a group.
 * @param obj  Target group. Must not be NULL.
 * @param mode New mode.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_mode(led_anim_obj_t *obj, led_anim_mode_t mode);

/**
 * @brief Set the enable mask of a group (bit i selects LED i).
 * @param obj  Target group. Must not be NULL.
 * @param mask New enable mask.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_mask(led_anim_obj_t *obj, uint32_t mask);

/**
 * @brief Set the maximum brightness level (ON level / BLINK upper bound).
 * @param obj   Target group. Must not be NULL.
 * @param level New maximum level.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_level_max(led_anim_obj_t *obj, led_anim_level_t level);

/**
 * @brief Set the minimum brightness level (BLINK lower bound).
 * @param obj   Target group. Must not be NULL.
 * @param level New minimum level.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_level_min(led_anim_obj_t *obj, led_anim_level_t level);

/**
 * @brief Set the ramp speed of a group.
 * @param obj       Target group. Must not be NULL.
 * @param ramp_step >0 = steps per ms, <0 = 1 step per |n| ms, 0 = instant.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_ramp_step(led_anim_obj_t *obj, int16_t ramp_step);

/**
 * @brief Set the blink dwell time at each extreme.
 * @param obj          Target group. Must not be NULL.
 * @param dead_time_ms Dwell time in ms before reversing the blink direction.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_blink_dead_time(led_anim_obj_t *obj, uint16_t dead_time_ms);

/**
 * @brief Force the current brightness of all enabled LEDs and aim toward max.
 *
 * Applies to LEDs selected by the enable mask only. Sets the current value to
 * @p value, refreshes the blink dwell counter, and points the target at
 * level_max.
 *
 * @param obj   Target group. Must not be NULL.
 * @param value New current level; must satisfy level_min <= value <= level_max.
 * @return ::LED_ANIM_OK on success,
 *         ::LED_ANIM_ERR_NULL if @p obj is NULL,
 *         ::LED_ANIM_ERR_RANGE if @p value is out of [level_min, level_max].
 */
led_anim_status_t led_anim_set_active_value(led_anim_obj_t *obj, led_anim_level_t value);

/**
 * @brief Set the ramp target of all enabled LEDs.
 *
 * Applies to LEDs selected by the enable mask only.
 *
 * @param obj   Target group. Must not be NULL.
 * @param value New target level; must satisfy level_min <= value <= level_max.
 * @return ::LED_ANIM_OK on success,
 *         ::LED_ANIM_ERR_NULL if @p obj is NULL,
 *         ::LED_ANIM_ERR_RANGE if @p value is out of [level_min, level_max].
 */
led_anim_status_t led_anim_set_active_target(led_anim_obj_t *obj, led_anim_level_t value);

/**
 * @brief Set the dwell counter of all disabled (masked-off) LEDs.
 * @param obj          Target group. Must not be NULL.
 * @param dead_time_ms Dwell value to load into the masked-off LEDs.
 * @return ::LED_ANIM_OK or ::LED_ANIM_ERR_NULL.
 */
led_anim_status_t led_anim_set_inactive_dead_time(led_anim_obj_t *obj, uint16_t dead_time_ms);

/**
 * @brief Advance the animation of every group by one tick and push PWM output.
 *
 * Call from a fixed 1 ms periodic context (SysTick / timer ISR).
 */
void led_anim_tick_1ms(void);

/**
 * @brief Release all groups and the LED pool back to their empty state.
 *
 * Primarily intended for unit tests and re-initialization. After this call all
 * previously returned handles are invalid.
 */
void led_anim_reset(void);

#endif /* LED_ANIM_H_ */
