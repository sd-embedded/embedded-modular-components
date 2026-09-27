/**
 * @file basic_usage.c
 * @brief Minimal usage example for the swtimer module.
 *
 * Demonstrates a periodic timer (a 500 ms heartbeat) and a one-shot timer
 * (a 2 s startup timeout). The example is HAL-independent: the two hardware
 * touch points — the 1 ms time base and whatever the callbacks act on — are
 * marked with TODO comments for the integrator to fill in.
 *
 * Wiring on a real target:
 *   1. Call setup_timers() once after start-up.
 *   2. Advance the timers once per millisecond by calling swtimer_tick_1ms().
 *      Either call it directly from a 1 ms interrupt (SysTick handler, RTOS
 *      tick hook, hardware timer ISR), or, when a callback does non-trivial
 *      work, let the 1 ms interrupt only record the elapsed time and call
 *      swtimer_tick_1ms() from the main loop. The deferred variant is
 *      recommended for anything but trivial callbacks: it keeps long callbacks
 *      out of interrupt context and, since the tick and the API then share one
 *      context, removes the need for a critical section (see swtimer.h).
 *   3. Run app_loop() from main().
 */

#include "swtimer.h"

/* Timer handles, obtained from swtimer_register(). */
static swtimer_t *heartbeat_timer;
static swtimer_t *startup_timeout_timer;

/*----------------------------------------------------------------------------
 * Callbacks
 *
 * Run from swtimer_tick_1ms(), so they execute in whichever context calls the
 * tick — an ISR in the direct variant, or the main loop in the deferred one.
 *--------------------------------------------------------------------------*/

/** @brief Fires every 500 ms while the periodic timer runs. */
static void on_heartbeat(void)
{
	/* TODO: toggle a status LED, kick a watchdog, etc. */
}

/** @brief Fires once, 2 s after the startup timeout timer is started. */
static void on_startup_timeout(void)
{
	/* TODO: react to the elapsed timeout (e.g. enter a safe/idle state). */
}

/*----------------------------------------------------------------------------
 * Setup
 *--------------------------------------------------------------------------*/

/**
 * @brief Register the application's timers and arm the ones that run from boot.
 * @return SWTIMER_OK on success, or the first error encountered.
 */
static swtimer_err_t setup_timers(void)
{
	swtimer_err_t register_result;

	/* Periodic: fire on_heartbeat() every 500 ms, forever. */
	const swtimer_config_t heartbeat_config = {
		.mode = SWTIMER_MODE_PERIODIC,
		.period = { .ms = 500, .sec = 0, .min = 0 },
		.callback = on_heartbeat,
	};

	register_result = swtimer_register(&heartbeat_config, &heartbeat_timer);
	if (register_result != SWTIMER_OK)
	{
		return register_result;
	}

	/* One-shot: fire on_startup_timeout() once, 2 s after it is started. */
	const swtimer_config_t startup_timeout_config = {
		.mode = SWTIMER_MODE_ONE_SHOT,
		.period = { .ms = 0, .sec = 2, .min = 0 },
		.callback = on_startup_timeout,
	};

	register_result = swtimer_register(&startup_timeout_config, &startup_timeout_timer);
	if (register_result != SWTIMER_OK)
	{
		return register_result;
	}

	/* Timers start stopped; arm the ones that should run from boot. */
	swtimer_start(heartbeat_timer);
	swtimer_start(startup_timeout_timer);

	return SWTIMER_OK;
}

/*----------------------------------------------------------------------------
 * 1 ms time base — direct variant
 *
 * Simplest wiring: call the tick straight from the 1 ms interrupt. Fine as long
 * as the callbacks are short (a flag, an LED, a counter).
 *--------------------------------------------------------------------------*/

/**
 * @brief Map this to a SysTick handler, RTOS tick hook, or 1 ms timer ISR.
 *
 * Advances every running timer and runs any due callbacks in ISR context.
 */
void time_base_1ms_isr(void)
{
	swtimer_tick_1ms();
}

/*----------------------------------------------------------------------------
 * 1 ms time base — deferred variant (recommended for non-trivial callbacks)
 *
 * Keep the ISR minimal and move the actual tick to the main loop. The 1 ms
 * interrupt only records that a millisecond elapsed; app_loop() drains the
 * pending milliseconds, so long callbacks run in thread context, not the ISR:
 *
 *   static volatile uint32_t pending_ms = 0;
 *
 *   void time_base_1ms_isr(void)   // 1 ms interrupt
 *   {
 *       pending_ms++;
 *   }
 *
 *   // inside app_loop():
 *   while (pending_ms > 0)
 *   {
 *       pending_ms--;              // make this read-modify-write ISR-safe
 *       swtimer_tick_1ms();        //   for your MCU (e.g. brief critical section)
 *   }
 *--------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
 * Application entry point
 *--------------------------------------------------------------------------*/

/** @brief Background loop; timers are driven entirely by the 1 ms tick. */
static void app_loop(void)
{
	for (;;)
	{
		/* Application work. Timer callbacks fire from the tick, not here. */

		/* Example of on-demand control: restart the timeout when needed. */
		/* swtimer_reset_and_start(startup_timeout_timer); */
	}
}

int main(void)
{
	/* TODO: platform init, then start the 1 ms time base feeding
	 * time_base_1ms_isr(). */

	if (setup_timers() != SWTIMER_OK)
	{
		/* TODO: handle registration failure (pool too small, bad config). */
		for (;;)
		{
		}
	}

	app_loop();
	return 0;
}
