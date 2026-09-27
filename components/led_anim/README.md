# led_anim

Portable, HAL-independent LED animation middleware for resource-constrained MCUs.

`led_anim` manages **groups of LEDs** and drives their brightness over time:
steady on/off and a smooth blink ("breathing") between a minimum and a maximum
level. Brightness changes are applied through ramps and pushed to the hardware
through a user-supplied PWM callback — one per LED. The module knows nothing
about your platform; you provide the PWM output function and a 1 ms tick.

## Features

- Three modes per group: **off**, **on**, and **blink** (smooth fade between two levels).
- Configurable ramp speed: fast (multiple steps per millisecond), slow (one step every *N* milliseconds), or instant.
- Per-LED enable mask (up to 32 LEDs per group).
- **No dynamic allocation** — objects and per-LED state live in static pools sized at compile time.
- **No HAL / platform headers** — output is a plain function pointer.
- Opaque object handle; all access goes through the public API.
- C99, Allman / BARR-C style, full Doxygen documentation.

## How it works

Each group has an animation `mode`, an enable `mask`, a `[level_min, level_max]`
range, a blink dwell time, and a ramp speed. On every 1 ms tick the module, for
each enabled LED:

1. picks a **target** level based on the mode (blink walks the target between
   `level_min` and `level_max`, pausing `blink_dead_ms` at each extreme);
2. advances the **current** level one ramp step toward the target;
3. calls the LED's PWM callback with the new current level.

The meaning of a "level" is yours to define: it is simply the value passed to
your PWM callback (e.g. a timer compare/ARR value). `0` is always off.

### Ramp speed (`ramp_step`)

| Value  | Behaviour                           |
|--------|-------------------------------------|
| `> 0`  | Move `ramp_step` levels per ms      |
| `< 0`  | Move 1 level every `|ramp_step|` ms |
| `0`    | Jump instantly to the target        |

## Configuration

Override these from your build system (e.g. `-DLED_ANIM_OBJECT_MAX=4`) or edit
`led_anim.h`:

| Macro                         | Default | Description                                        |
|-------------------------------|---------|----------------------------------------------------|
| `LED_ANIM_OBJECT_MAX`         | `11`    | Maximum number of LED groups in the static pool.   |
| `LED_ANIM_LED_POOL_SIZE`      | `11`    | Total per-LED slots shared by all groups.          |
| `LED_ANIM_MAX_LEDS_PER_GROUP` | `32`    | LEDs per group, bounded by the 32-bit enable mask. |

## Integration

1. Add `led_anim.c` / `led_anim.h` to your build.
2. Create one or more groups and attach a PWM callback to each LED.
3. Call `led_anim_tick_1ms()` from a fixed 1 ms context (SysTick or a timer ISR).

### Concurrency

`led_anim_tick_1ms()` is meant to run from the 1 ms ISR; every other function
runs from main/thread context. State is protected by a cooperative `busy` flag:
a writer raises it for the whole operation and the tick skips a cycle if it
finds the flag raised. This keeps the module free of interrupt-masking
intrinsics. The only side effect is that a tick coinciding with a writer may be
dropped — harmless for LED timing.

## Usage example

A complete, host-runnable example is in [`basic_usage.c`](basic_usage.c):

```sh
gcc -std=c99 -Wall -Wextra basic_usage.c led_anim.c -o basic_usage
./basic_usage
```

The essential wiring:

```c
#include "led_anim.h"

/* Map a 0..1000 level onto your PWM hardware. */
static void led0_pwm(led_anim_level_t level)
{
	timer_set_compare(TIM_CH1, level);
}

static led_anim_obj_t *status_leds;

void leds_init(void)
{
	const led_anim_config_t config =
	{
		.mode          = LED_ANIM_MODE_BLINK,
		.num_leds      = 1u,
		.mask          = 0x01u,
		.level_max     = 1000u,
		.level_min     = 0u,
		.blink_dead_ms = 200u,
		.ramp_step     = 5,     /* +5 levels per ms */
	};

	if (led_anim_create(&config, &status_leds) != LED_ANIM_OK)
	{
		return; /* bad config or pool exhausted */
	}

	led_anim_attach_pwm(status_leds, 0u, led0_pwm);
}

/* Call exactly once per millisecond. */
void SysTick_Handler(void)
{
	led_anim_tick_1ms();
}
```

## API summary

### Lifecycle

| Function                              | Description                                        |
|---------------------------------------|----------------------------------------------------|
| `led_anim_create(config, out_obj)`    | Create a group from a configuration.               |
| `led_anim_attach_pwm(obj, index, fn)` | Attach the PWM output callback for one LED.        |
| `led_anim_reset(void)`                | Release all groups (mainly for tests / re-init).   |
| `led_anim_tick_1ms(void)`             | Advance every group by one tick; call every 1 ms.  |

### Group configuration

| Function                                | Description                              |
|-----------------------------------------|------------------------------------------|
| `led_anim_set_mode(obj, mode)`          | Set the animation mode.                  |
| `led_anim_set_mask(obj, mask)`          | Set the enable mask (bit *i* = LED *i*). |
| `led_anim_set_level_max(obj, level)`    | Set the ON level / blink upper bound.    |
| `led_anim_set_level_min(obj, level)`    | Set the blink lower bound.               |
| `led_anim_set_ramp_step(obj, step)`     | Set the ramp speed (see table above).    |
| `led_anim_set_blink_dead_time(obj, ms)` | Set the dwell time at each blink extreme.|

### Runtime control

| Function                                    | Description                                              |
|---------------------------------------------|----------------------------------------------------------|
| `led_anim_set_active_value(obj, value)`     | Force the current level of enabled LEDs; aim toward max. |
| `led_anim_set_active_target(obj, value)`    | Set the ramp target of enabled LEDs.                     |
| `led_anim_set_inactive_dead_time(obj, ms)`  | Set the dwell counter of disabled LEDs.                  |

### Return codes (`led_anim_status_t`)

| Code                     | Meaning                                |
|--------------------------|----------------------------------------|
| `LED_ANIM_OK`            | Success.                               |
| `LED_ANIM_ERR_NULL`      | A required pointer argument was NULL.  |
| `LED_ANIM_ERR_NO_MEMORY` | Object pool or LED pool exhausted.     |
| `LED_ANIM_ERR_RANGE`     | A value or count was outside range.    |
| `LED_ANIM_ERR_INDEX`     | LED index out of range for the group.  |

## File structure

```
led_anim.h      Public API and compile-time configuration
led_anim.c      Implementation
basic_usage.c   Standalone, host-runnable usage example
README.md       This file
```

## Requirements

- A C99 compiler.
- A 1 ms periodic interrupt or timer to drive `led_anim_tick_1ms()`.
- A PWM (or on/off) output function per LED.

## License

MIT — see [LICENSE](LICENSE).
