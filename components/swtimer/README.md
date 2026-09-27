# swtimer

A small, portable **software timer** middleware for resource-constrained MCUs.
Timers count down in millisecond / second / minute units and invoke a user
callback when they reach zero — one-shot or periodic. The module is fully
HAL-independent: the only thing you wire up is a 1 ms time base.

> Part of a HAL-independent embedded C middleware library. C99, no dynamic
> allocation, no platform headers.

---

## Features

- **One-shot and periodic** timers (`SWTIMER_MODE_ONE_SHOT`, `SWTIMER_MODE_PERIODIC`).
- **Static object pool** — no `malloc`/`free`. Pool size fixed at compile time.
- **HAL-independent** — no vendor headers; you provide the 1 ms tick.
- **Explicit run states** — `STOPPED`, `RUNNING`, `PAUSED`, with distinct
  stop (reload) vs pause (freeze) semantics.
- **Error codes on every fallible call** — no silent failures, NULL-checked API.
- **Doxygen-documented** public API.
- **C99**, portable across GCC / IAR / Keil / MSVC (no compiler extensions).

---

## Configuration

All configuration is compile-time, via the header or a `-D` build flag.

| Macro                | Default | Description                                  |
|----------------------|---------|----------------------------------------------|
| `SWTIMER_POOL_SIZE`  | `8`     | Maximum number of timers in the static pool. |

Override without editing the header:

```
-DSWTIMER_POOL_SIZE=16
```

---

## Usage

```c
#include "swtimer.h"

static swtimer_t *heartbeat;

static void on_heartbeat(void) {
    /* toggle an LED, kick a watchdog, ... */
}

void app_init(void) {
    const swtimer_config_t cfg = {
        .mode     = SWTIMER_MODE_PERIODIC,
        .period   = { .ms = 500, .sec = 0, .min = 0 },  /* 500 ms */
        .callback = on_heartbeat,
    };

    if (swtimer_register(&cfg, &heartbeat) == SWTIMER_OK)
        swtimer_start(heartbeat);
}

/* Call once per millisecond from your time base (SysTick, RTOS tick, timer ISR). */
void SysTick_Handler(void) {
    swtimer_tick_1ms();
}
```

See [`basic_usage.c`](basic_usage.c) for a fuller example with both a periodic
and a one-shot timer, including the deferred (main-loop) tick variant.

### Integration checklist

1. Add `swtimer.c` to your build and `swtimer.h` to your include path.
2. Call `swtimer_register()` for each timer at start-up, then `swtimer_start()`
   (or `swtimer_reset_and_start()`).
3. Call `swtimer_tick_1ms()` **exactly once per millisecond** from a stable
   1 ms time base.

> **Note:** callbacks run in the context of `swtimer_tick_1ms()` — usually an
> ISR. Keep them short and ISR-safe.

---

## Timer states

| State                    | Counting? | On `swtimer_start()`        |
|--------------------------|-----------|-----------------------------|
| `SWTIMER_STATE_STOPPED`  | no        | starts from a full reload   |
| `SWTIMER_STATE_RUNNING`  | yes       | (already running)           |
| `SWTIMER_STATE_PAUSED`   | no        | resumes from remaining time |

- **`swtimer_stop()`** reloads the timer, so a later `start` runs a full period.
- **`swtimer_pause()`** freezes the remaining time, so a later `start` resumes it.

---

## API summary

| Function                    | Purpose                                             |
|-----------------------------|-----------------------------------------------------|
| `swtimer_register()`        | Allocate a timer from the pool and configure it.    |
| `swtimer_set_time()`        | Change the reload duration (applies on next reload).|
| `swtimer_start()`           | Start a stopped timer or resume a paused one.       |
| `swtimer_stop()`            | Stop and reload to the initial value.               |
| `swtimer_pause()`           | Freeze, preserving remaining time.                  |
| `swtimer_reset()`           | Reload remaining time, keep the run state.          |
| `swtimer_reset_and_start()` | Reload and start in one call.                       |
| `swtimer_get_state()`       | Query the current run state.                        |
| `swtimer_tick_1ms()`        | Advance all timers by 1 ms; call from the tick.     |
| `swtimer_pool_clear()`      | Clear the whole pool (mainly for unit tests).       |

### Error codes (`swtimer_err_t`)

| Code                       | Meaning                                        |
|----------------------------|------------------------------------------------|
| `SWTIMER_OK`               | Success.                                       |
| `SWTIMER_ERR_NULL_CONFIG`  | Configuration pointer was NULL.                |
| `SWTIMER_ERR_NULL_TIMER`   | Timer (or output) handle was NULL.             |
| `SWTIMER_ERR_POOL_FULL`    | No free slot left in the static pool.          |
| `SWTIMER_ERR_ZERO_PERIOD`  | Period was zero (would expire every tick).     |

---

## Design notes

- **No dynamic allocation.** All timers live in a single static array sized by
  `SWTIMER_POOL_SIZE`; `swtimer_register()` hands out slots in order.
- **Zero period is rejected.** A `{0, 0, 0}` period would fire the callback on
  every tick, which is almost always a configuration mistake, so both
  `swtimer_register()` and `swtimer_set_time()` reject it with
  `SWTIMER_ERR_ZERO_PERIOD`. The smallest valid period is `1 ms`.
- **The tick only touches running timers**, and iterates only over registered
  slots, not the whole pool.
- **Resolution** is 1 ms, set entirely by how often `swtimer_tick_1ms()` is
  called. Callback timing accuracy is bounded by that time base's jitter.

---

## Files

| File            | Description                          |
|-----------------|--------------------------------------|
| `swtimer.h`     | Public API and configuration.        |
| `swtimer.c`     | Implementation.                      |
| `basic_usage.c` | Usage example.                       |
| `README.md`     | This file.                           |

---

## Requirements

- A C99 compiler.
- A 1 ms time base that calls `swtimer_tick_1ms()` (SysTick, an RTOS tick hook,
  or a hardware timer interrupt).
