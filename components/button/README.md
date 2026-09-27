# button

A portable, HAL-independent button/key handler for resource-constrained MCUs.

`button` turns raw, polled button samples into meaningful events — short press,
long hold, and auto-repeat while held — through a small, callback-driven API. It
has no dependency on any vendor HAL, no dynamic memory allocation, and targets
plain C99, so it drops into bare-metal and RTOS projects alike.

## Features

- **Three event types per button**, freely combined:
  - fire once when released within a time window (short / medium / long press),
  - fire once after a minimum hold time is reached,
  - fire repeatedly at a fixed period while the button is held.
- **Configurable release debounce** per button.
- **Optional cross-button event lockout** (`blocks_others`): when one event
  fires, all other one-shot events are suppressed until every button is released
  — useful when a long hold must cancel any other reaction.
- **Static allocation only.** All state lives in fixed-size pools; pool sizes are
  compile-time configurable.
- **HAL-independent and C99.** No platform headers, no compiler extensions, no
  `malloc`/`free`. The only things the module needs from the application are a
  1 ms tick and a per-scan "pressed" report.
- **Fully documented** public API (Doxygen).

## How it works

The application drives the module with two signals:

1. **`button_tick_1ms()`** — call exactly once per millisecond (e.g. from a
   SysTick / 1 ms timer interrupt). This advances every button's state machine.
2. **`button_report_pressed()`** — call from your input-scanning code on every
   scan in which a button samples as *pressed*.

Press duration is measured from the cadence of these two calls. As long as the
scan runs at (or near) the 1 ms tick rate, `pressed_time_ms` tracks real elapsed
hold time. When press reports stop arriving, the release is confirmed after the
configured debounce window and release events are evaluated.

## Configuration

Override these before including `button.h` (e.g. via compiler `-D` flags):

| Macro                    | Default | Description                                      |
|--------------------------|---------|--------------------------------------------------|
| `BUTTON_MAX_COUNT`       | `6`     | Maximum number of buttons in the static pool.    |
| `BUTTON_EVENT_MAX_COUNT` | `12`    | Maximum number of events shared across buttons.  |

## Event modes

| Mode                              | Fires                                              | Uses fields                  |
|-----------------------------------|----------------------------------------------------|------------------------------|
| `BUTTON_EVENT_ON_RELEASE`         | Once on release, if hold time ∈ `[min, max)`       | `min_hold_time_ms`, `max_hold_time_ms` |
| `BUTTON_EVENT_ON_HOLD`            | Once when hold time first reaches `min`            | `min_hold_time_ms`                     |
| `BUTTON_EVENT_REPEAT_WHILE_HELD`  | Every `period` once hold time reaches `min`        | `min_hold_time_ms`, `repeat_period_ms` |

Set `blocks_others = true` on any one-shot event to suppress all other one-shot
events (across all buttons) until every button returns to idle.
`BUTTON_EVENT_REPEAT_WHILE_HELD` events are never blocked and never block others.

## Usage

```c
#include "button.h"

/* --- Application-provided callbacks ------------------------------------- */

static void on_short_press(void)  { /* toggle something          */ }
static void on_long_hold(void)    { /* enter configuration mode  */ }
static void on_repeat(void)       { /* increment a value         */ }

/* --- Application-provided I/O ------------------------------------------- */

extern bool board_button_is_down(void); /* reads the GPIO pin */

static button_t *btn;

/* --- Setup -------------------------------------------------------------- */

void buttons_init(void)
{
	button_create(&btn);
	button_set_debounce(btn, 20); /* 20 ms release debounce */

	/* Short press: released between 20 ms and 500 ms. */
	button_event_t short_press = {
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_ON_RELEASE,
		.min_hold_time_ms = 20,
		.max_hold_time_ms = 500,
		.callback         = on_short_press,
	};
	button_add_event(btn, &short_press);

	/* Long hold: fires once after 1 s; cancels the short press for this gesture. */
	button_event_t long_hold = {
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_ON_HOLD,
		.min_hold_time_ms = 1000,
		.blocks_others    = true,
		.callback         = on_long_hold,
	};
	button_add_event(btn, &long_hold);

	/* Auto-repeat: every 200 ms once held for 1 s. */
	button_event_t repeat = {
		.status           = BUTTON_EVENT_ENABLED,
		.mode             = BUTTON_EVENT_REPEAT_WHILE_HELD,
		.min_hold_time_ms = 1000,
		.repeat_period_ms = 200,
		.callback         = on_repeat,
	};
	button_add_event(btn, &repeat);
}

/* --- Runtime ------------------------------------------------------------ */

/* Call once per millisecond, e.g. from the SysTick handler. */
void on_systick_1ms(void)
{
	if (board_button_is_down())
	{
		button_report_pressed(btn);
	}

	button_tick_1ms();
}
```

## API summary

| Function                  | Description                                              |
|---------------------------|----------------------------------------------------------|
| `button_create`           | Allocate and initialise a button from the static pool.   |
| `button_set_debounce`     | Set the release debounce window for a button.            |
| `button_add_event`        | Attach an event (copied into the static pool).           |
| `button_report_pressed`   | Report that a button is currently sampled as pressed.    |
| `button_reset_hold_time`  | Reset a button's accumulated press duration.             |
| `button_tick_1ms`         | Advance all button state machines by one millisecond.    |

All functions that can fail return a `button_status_t`:

| Code                       | Meaning                                  |
|----------------------------|------------------------------------------|
| `BUTTON_OK`                | Success.                                 |
| `BUTTON_ERR_NO_MEMORY`     | Static pool exhausted.                   |
| `BUTTON_ERR_NULL_CONFIG`   | A required pointer argument was `NULL`.  |
| `BUTTON_ERR_INVALID_PARAM` | Invalid argument or event configuration. |

## Integration notes

- `button_report_pressed()` and `button_tick_1ms()` are typically called from the
  same 1 ms context. If you scan inputs in the main loop instead, ensure the scan
  rate is fast and stable enough for the timing accuracy you need.
- The module is not internally synchronised. If `button_tick_1ms()` runs in an
  ISR while configuration calls (`button_create`, `button_add_event`, ...) run in
  the main context, perform configuration before enabling the tick, or guard the
  shared state in the application.

## License

Released under the MIT License. Copyright © sd-embedded.
