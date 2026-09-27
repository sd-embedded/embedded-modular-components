/**
 * @file swtimer.h
 * @brief Portable software timer middleware (countdown timers with callbacks).
 *
 * swtimer provides a small set of software timers that count down in
 * millisecond / second / minute units and invoke a user callback when they
 * reach zero. Timers can run once (one-shot) or reload automatically
 * (periodic). The module is fully HAL-independent: the integrator only has to
 * call swtimer_tick_1ms() from a 1 ms time base (e.g. SysTick) and let the
 * registered callbacks run from there.
 *
 * Memory model: static object pool only, no dynamic allocation. The pool size
 * is configurable through SWTIMER_POOL_SIZE.
 *
 * @note swtimer_tick_1ms() executes callbacks in its own context (usually an
 *       ISR or a periodic task). Keep callbacks short and re-entrancy aware.
 *
 * @note The module holds no internal locking. swtimer_tick_1ms() and the API
 *       functions all touch the shared timer pool, so when the tick runs in an
 *       ISR while the API is called from thread/main context, guard the API
 *       calls (or the tick) with a critical section.
 */

#ifndef SWTIMER_H
#define SWTIMER_H

#include <stdint.h>

/**
 * @brief Maximum number of timers held in the static pool.
 *
 * Override at build time (e.g. -DSWTIMER_POOL_SIZE=16) to fit the application.
 */
#ifndef SWTIMER_POOL_SIZE
#define SWTIMER_POOL_SIZE 8
#endif

/** @brief Result codes returned by the swtimer API. */
typedef enum
{
	SWTIMER_OK = 0,          /**< Operation completed successfully. */
	SWTIMER_ERR_NULL_CONFIG, /**< Configuration pointer was NULL. */
	SWTIMER_ERR_NULL_TIMER,  /**< Timer handle was NULL. */
	SWTIMER_ERR_POOL_FULL,   /**< No free slot left in the static pool. */
	SWTIMER_ERR_ZERO_PERIOD, /**< Period was zero (would expire every tick). */
} swtimer_err_t;

/** @brief Expiry behaviour of a timer. */
typedef enum
{
	SWTIMER_MODE_ONE_SHOT = 0, /**< Fire once, then stop. */
	SWTIMER_MODE_PERIODIC,     /**< Fire and auto-reload from the initial value. */
} swtimer_mode_t;

/** @brief Run state of a timer. */
typedef enum
{
	SWTIMER_STATE_STOPPED = 0, /**< Not counting; reloaded and ready to start. */
	SWTIMER_STATE_RUNNING,     /**< Counting down on every tick. */
	SWTIMER_STATE_PAUSED,      /**< Frozen; resumes from the current value. */
} swtimer_state_t;

/**
 * @brief Callback invoked from swtimer_tick_1ms() when a timer expires.
 *
 * Runs in the tick context. Must be short and ISR-safe if the tick is driven
 * from an interrupt.
 */
typedef void (*swtimer_callback_t)(void);

/** @brief Time value expressed in minutes, seconds and milliseconds. */
typedef struct
{
	uint16_t ms;  /**< Milliseconds component (0..999). */
	uint16_t sec; /**< Seconds component (0..59). */
	uint16_t min; /**< Minutes component. */
} swtimer_time_t;

/** @brief Parameters supplied by the caller when registering a timer. */
typedef struct
{
	swtimer_mode_t     mode;     /**< One-shot or periodic. */
	swtimer_time_t     period;   /**< Initial / reload duration; must be non-zero. */
	swtimer_callback_t callback; /**< Function called on expiry (may be NULL). */
} swtimer_config_t;

/**
 * @brief Software timer handle.
 *
 * Allocated from the internal static pool by swtimer_register(). Treat the
 * fields as read-only and use the API to change a timer's behaviour.
 */
typedef struct
{
	swtimer_mode_t     mode;     /**< Expiry behaviour. */
	swtimer_state_t    state;    /**< Current run state. */
	swtimer_time_t     initial;  /**< Reload value. */
	swtimer_time_t     current;  /**< Remaining time. */
	swtimer_callback_t callback; /**< Expiry callback. */
} swtimer_t;

/**
 * @brief Register a new timer in the static pool.
 *
 * Copies @p config into a free pool slot, loads the countdown value from
 * config->period and leaves the timer in SWTIMER_STATE_STOPPED. Start it with
 * swtimer_start() or swtimer_reset_and_start().
 *
 * @param[in]  config     Timer configuration. Must not be NULL.
 * @param[out] out_timer  Receives the handle of the created timer on success.
 *                        Must not be NULL.
 * @return SWTIMER_OK on success,
 *         SWTIMER_ERR_NULL_CONFIG if @p config is NULL,
 *         SWTIMER_ERR_NULL_TIMER if @p out_timer is NULL,
 *         SWTIMER_ERR_ZERO_PERIOD if config->period is zero,
 *         SWTIMER_ERR_POOL_FULL if the pool has no free slot.
 */
swtimer_err_t swtimer_register(const swtimer_config_t *config, swtimer_t **out_timer);

/**
 * @brief Set (or change) the reload duration of a timer.
 *
 * Updates the initial/reload value. The change takes effect on the next reload
 * (swtimer_reset(), swtimer_reset_and_start(), swtimer_stop(), or the automatic
 * reload of a periodic timer). A currently running countdown is not altered.
 *
 * @param timer Timer handle. Must not be NULL.
 * @param ms    Milliseconds component (0..999).
 * @param sec   Seconds component (0..59).
 * @param min   Minutes component.
 * @return SWTIMER_OK, SWTIMER_ERR_NULL_TIMER, or SWTIMER_ERR_ZERO_PERIOD if all
 *         three components are zero.
 */
swtimer_err_t swtimer_set_time(swtimer_t *timer, uint16_t ms, uint16_t sec, uint16_t min);

/**
 * @brief Start or resume a timer without reloading it.
 *
 * Sets the state to SWTIMER_STATE_RUNNING. The countdown continues from the
 * current remaining time, so this both starts a freshly registered/reset timer
 * and resumes a paused one.
 *
 * @param timer Timer handle. Must not be NULL.
 * @return SWTIMER_OK or SWTIMER_ERR_NULL_TIMER.
 */
swtimer_err_t swtimer_start(swtimer_t *timer);

/**
 * @brief Stop a timer and reload it.
 *
 * Sets the state to SWTIMER_STATE_STOPPED and reloads the remaining time from
 * the initial value, so a subsequent swtimer_start() runs a full period.
 *
 * @param timer Timer handle. Must not be NULL.
 * @return SWTIMER_OK or SWTIMER_ERR_NULL_TIMER.
 */
swtimer_err_t swtimer_stop(swtimer_t *timer);

/**
 * @brief Pause a running timer, preserving its remaining time.
 *
 * Sets the state to SWTIMER_STATE_PAUSED. Resume with swtimer_start() to
 * continue from where it stopped.
 *
 * @param timer Timer handle. Must not be NULL.
 * @return SWTIMER_OK or SWTIMER_ERR_NULL_TIMER.
 */
swtimer_err_t swtimer_pause(swtimer_t *timer);

/**
 * @brief Reload the remaining time from the initial value.
 *
 * Does not change the run state.
 *
 * @param timer Timer handle. Must not be NULL.
 * @return SWTIMER_OK or SWTIMER_ERR_NULL_TIMER.
 */
swtimer_err_t swtimer_reset(swtimer_t *timer);

/**
 * @brief Reload the timer and start it in one call.
 *
 * Equivalent to swtimer_reset() followed by swtimer_start().
 *
 * @param timer Timer handle. Must not be NULL.
 * @return SWTIMER_OK or SWTIMER_ERR_NULL_TIMER.
 */
swtimer_err_t swtimer_reset_and_start(swtimer_t *timer);

/**
 * @brief Get the current run state of a timer.
 *
 * @param timer Timer handle.
 * @return The timer's state, or SWTIMER_STATE_STOPPED if @p timer is NULL.
 */
swtimer_state_t swtimer_get_state(const swtimer_t *timer);

/**
 * @brief Advance all registered timers by one millisecond.
 *
 * Call exactly once per millisecond from a stable time base (SysTick handler,
 * RTOS tick hook, etc.). Decrements every running timer and invokes the
 * callback of any timer that reaches zero, then reloads it (periodic) or stops
 * it (one-shot).
 */
void swtimer_tick_1ms(void);

/**
 * @brief Clear the entire timer pool.
 *
 * Removes all registered timers and frees every pool slot. Intended mainly for
 * unit tests (e.g. Unity setUp); typical firmware registers timers once at
 * start-up and never needs this.
 */
void swtimer_pool_clear(void);

#endif /* SWTIMER_H */
