/**
 * @file button.h
 * @brief Portable, HAL-independent button handler with a per-button event system.
 *
 * This middleware provides debounced button handling with a flexible,
 * callback-driven event model. Each button owns a list of events; an event
 * fires its callback when a configured timing condition is met (hold time
 * reached, released within a time window, or repeated while held).
 *
 * The module is fully platform-independent and uses static object pools only
 * (no dynamic allocation). The application is responsible for:
 *   - calling ::button_tick_1ms exactly once every millisecond
 *     (e.g. from a 1 ms timer / SysTick handler), and
 *   - reporting the physical state of each button via ::button_report_pressed
 *     on every scan in which it samples as pressed.
 *
 * @note All pool sizes can be overridden from the build system by predefining
 *       the corresponding macro before this header is included.
 *
 * @author Szymon
 */

#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Maximum number of buttons that can be created from the static pool. */
#ifndef BUTTON_MAX_COUNT
#define BUTTON_MAX_COUNT 6
#endif

/** @brief Maximum number of events (shared across all buttons) in the pool. */
#ifndef BUTTON_EVENT_MAX_COUNT
#define BUTTON_EVENT_MAX_COUNT 12
#endif

/* -------------------------------------------------------------------------- */
/* Return codes                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Return codes used across the public API.
 */
typedef enum
{
	BUTTON_OK = 0,             /**< Operation completed successfully.          */
	BUTTON_ERR_NO_MEMORY,      /**< Static pool exhausted.                     */
	BUTTON_ERR_NULL_CONFIG,    /**< A required configuration pointer was NULL. */
	BUTTON_ERR_INVALID_PARAM   /**< A parameter held an invalid value.         */
}button_status_t;

/* -------------------------------------------------------------------------- */
/* Event model                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief Enable flag for an event (set by the user in the event config).
 */
typedef enum
{
	BUTTON_EVENT_DISABLED = 0, /**< Event is ignored by the tick handler.   */
	BUTTON_EVENT_ENABLED       /**< Event is evaluated by the tick handler. */
}button_event_status_t;

/**
 * @brief Condition that triggers an event callback.
 */
typedef enum
{
	/**
	 * @brief Fires once on release, if the press duration was within the
	 *        half-open window [min_hold_time_ms, max_hold_time_ms).
	 *        Uses @c min_hold_time_ms and @c max_hold_time_ms.
	 */
	BUTTON_EVENT_ON_RELEASE,

	/**
	 * @brief Fires once when the press duration first reaches @c min_hold_time_ms.
	 *        Uses @c min_hold_time_ms.
	 */
	BUTTON_EVENT_ON_HOLD,

	/**
	 * @brief Fires repeatedly every @c repeat_period_ms once the press duration
	 *        has reached @c min_hold_time_ms.
	 *        Uses @c min_hold_time_ms and @c repeat_period_ms.
	 */
	BUTTON_EVENT_REPEAT_WHILE_HELD
}button_event_mode_t;

/** @brief Callback invoked when an event condition is met. */
typedef void (*button_event_callback_t)(void);

/**
 * @brief A single button event descriptor.
 *
 * Events are stored as a NULL-terminated singly-linked list owned by the
 * button. The user fills the configuration fields (@c status, @c mode, the
 * timing fields, @c callback and @c blocks_others) in a local instance and
 * passes it to ::button_add_event, which copies it into the static pool. The
 * remaining fields are managed internally and must not be set by the user.
 */
typedef struct button_event
{
	button_event_status_t   status;            /**< Enable/disable flag (user config).          */
	button_event_mode_t     mode;              /**< Trigger condition (user config).            */
	uint32_t                min_hold_time_ms;  /**< Minimum hold time, ms (mode dependent).     */
	uint32_t                max_hold_time_ms;  /**< Maximum hold time, ms (ON_RELEASE only).    */
	uint32_t                repeat_period_ms;  /**< Repeat period, ms (REPEAT_WHILE_HELD only). */
	button_event_callback_t callback;          /**< Function invoked when the event fires.      */
	bool                    blocks_others;     /**< User config: on fire, suppress all other
	                                                one-shot events until all buttons are idle.  */
	bool                    is_blocked;        /**< Runtime: suppressed by another event.       */
	bool                    has_fired;         /**< Runtime: ON_HOLD fired-once latch.          */
	struct button_event    *next;              /**< Runtime: next event in the list, or NULL.   */
}button_event_t;

/* -------------------------------------------------------------------------- */
/* Button                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal state of a button's debounce / event state machine.
 */
typedef enum
{
	BUTTON_STATE_IDLE = 0,    /**< No press in progress.                         */
	BUTTON_STATE_PRESSED,     /**< Press reported; pending evaluation this tick. */
	BUTTON_STATE_WAIT_RELEASE /**< Press handled; debouncing the release edge.   */
}button_state_t;

/**
 * @brief A single button instance.
 *
 * Allocated from the static pool by ::button_create. All fields are managed
 * internally; treat the handle as opaque from application code and configure
 * it only through the public API.
 */
typedef struct button
{
	button_state_t  state;                /**< Current state-machine state.       */
	uint32_t        hold_time_ms;         /**< Accumulated press duration, ms.    */
	uint16_t        debounce_time_ms;     /**< Release debounce window, ms.       */
	uint16_t        debounce_elapsed_ms;  /**< Runtime release-debounce counter.  */
	button_event_t *events;               /**< Head of this button's event list.  */
}button_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Allocate and initialise a single button from the static pool.
 *
 * The button starts in ::BUTTON_STATE_IDLE with a zero debounce window and an
 * empty event list. Set debouncing with ::button_set_debounce and attach
 * behaviour with ::button_add_event.
 *
 * @param[out] created_button Receives a pointer to the created button on success.
 * @return ::BUTTON_OK on success,
 *         ::BUTTON_ERR_INVALID_PARAM if @p created_button is NULL,
 *         ::BUTTON_ERR_NO_MEMORY if the button pool is exhausted.
 */
button_status_t button_create(button_t **created_button);

/**
 * @brief Set the release debounce window for a button.
 *
 * Once a button stops being reported as pressed, the release is accepted only
 * after @p debounce_time_ms consecutive ticks without a press report. A value
 * of 0 accepts the release on the first such tick.
 *
 * @param[in] button           Target button (NULL is ignored).
 * @param[in] debounce_time_ms Debounce window in milliseconds.
 */
void button_set_debounce(button_t *button, uint16_t debounce_time_ms);

/**
 * @brief Attach an event to a button.
 *
 * The descriptor is copied into the static event pool, so the caller's
 * @p config instance does not need to persist after this call.
 *
 * @param[in] button Target button.
 * @param[in] config Event configuration to copy.
 * @return ::BUTTON_OK on success,
 *         ::BUTTON_ERR_NULL_CONFIG if @p button or @p config is NULL,
 *         ::BUTTON_ERR_INVALID_PARAM if the timing parameters are invalid for
 *           the selected mode (e.g. @c repeat_period_ms == 0 in
 *           ::BUTTON_EVENT_REPEAT_WHILE_HELD),
 *         ::BUTTON_ERR_NO_MEMORY if the event pool is exhausted.
 */
button_status_t button_add_event(button_t *button, const button_event_t *config);

/**
 * @brief Report that a button is currently sampled as pressed.
 *
 * Call this from the application's input-scanning code on every scan in which
 * the physical button reads as pressed. The module derives press duration and
 * release detection from the cadence of these reports relative to
 * ::button_tick_1ms.
 *
 * @param[in] button Target button (NULL is ignored).
 */
void button_report_pressed(button_t *button);

/**
 * @brief Reset a button's accumulated press duration to zero.
 *
 * @param[in] button Target button (NULL is ignored).
 */
void button_reset_hold_time(button_t *button);

/**
 * @brief Advance every button's state machine by one millisecond.
 *
 * Must be called exactly once per millisecond. Evaluates each button's events
 * and invokes their callbacks when their conditions are met.
 */
void button_tick_1ms(void);

#endif /* BUTTON_H_ */
