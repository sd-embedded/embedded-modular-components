# uart

A small, portable, **HAL-independent UART framing middleware** for embedded C.
It handles frame parsing, per-port circular receive buffers, CRC8/CRC16 and
Modbus RTU length detection, in both **master** and **slave** roles — with no
dynamic memory and no platform headers.

![C99](https://img.shields.io/badge/C-C99-blue)
![no malloc](https://img.shields.io/badge/allocation-static%20only-green)
![HAL](https://img.shields.io/badge/HAL-independent-orange)
![license](https://img.shields.io/badge/license-MIT-lightgrey)

---

## Why

Most UART protocol code ends up tangled with a specific HAL (STM32 HAL, ESP-IDF,
PSoC, …) and re-implemented per project. This module separates the *protocol*
from the *hardware*: you provide a byte-send function and (optionally) a CRC
routine via function pointers, and the module does the framing. The same source
compiles unchanged on STM32, PSoC4, ESP32, STM8 or a host build.

It also scales past a single link: the module manages **several independent UART
ports**, and **each port can carry several different frame formats** at once
(e.g. a legacy fixed-length protocol and Modbus RTU on the same bus), each with
its own header, length rule, CRC and handlers.

## Features

- **Static object pools only** — no `malloc`/`free`.
- **Multiple ports, multiple frame formats per port** — each device on a port
  has its own frame specification and callbacks.
- **Frame parser** with three length strategies: fixed, in-frame length byte,
  and Modbus RTU (length derived from the function code).
- **Pluggable CRC** — CRC8 or CRC16, passed in explicitly (re-entrant, no global
  state).
- **Master and slave** roles on the same API.
- **Robust RX buffering** — circular buffer with per-byte time-to-live and an
  optional bus-idle timeout that flushes stale partial frames.
- **Configurable buffer sizes** via header defines.

## How it works

```
   ISR / DMA                 super-loop                 1 ms tick
      │                          │                          │
 uart_receive() ─► incoming ─────┤                          │
                  bytes          │  uart_task()             │  uart_tick_1ms()
                                 ▼                          ▼
                    move to bytes_to_process        age bytes / idle flush
                                 │
                                 ▼
                          uart_parse_frame()
                                 │  (valid frame)
                                 ▼
                    process_frame() ─► build_frame() ─► send_bytes()
```

Bytes arrive in an ISR and are queued with `uart_receive()`. `uart_task()`, run
from the main loop, moves them into a processing buffer, parses frames per
device, and on a valid frame invokes your `process_frame` callback (and, for a
reply, `build_frame` + the low-level sender). `uart_tick_1ms()` ages buffered
bytes and applies the idle timeout.

## Configuration

All sizes live in `uart.h`:

| Macro                   | Default | Meaning                                   |
|-------------------------|:-------:|-------------------------------------------|
| `UART_MAX_COUNT`        | `1`     | Number of UART ports in the static pool   |
| `UART_DEVICE_MAX_COUNT` | `2`     | Number of devices in the static pool      |
| `UART_RX_BUFFER_SIZE`   | `64`    | Receive circular buffer size (bytes)      |
| `UART_TX_BUFFER_SIZE`   | `64`    | Transmit buffer size (bytes)              |

> Frame lengths are carried in a single byte and are therefore capped at 255.
> The RX/TX buffers may be larger than that.

## Integration (3 hooks)

```c
/* 1) From the UART RX interrupt / DMA-complete callback: */
void on_uart_rx(const uint8_t *bytes, uint8_t count)
{
    uart_receive(port, bytes, count);
}

/* 2) Once per millisecond (e.g. SysTick): */
void on_systick_1ms(void)
{
    uart_tick_1ms();
}

/* 3) In the main super-loop: */
for (;;)
{
    uart_task();
    /* ... */
}
```

## Frame specification

A device describes the frames it expects via `uart_rx_frame_spec_t`.

**Header** (`header_type`)

| Value                  | Meaning                          |
|------------------------|----------------------------------|
| `UART_HEADER_NONE`     | No header byte                   |
| `UART_HEADER_ONE_BYTE` | Frame starts with `header_byte`  |

**Length** (`length_type`)

| Value                 | Meaning                                          |
|-----------------------|--------------------------------------------------|
| `UART_LENGTH_CONST`   | Fixed length (`length`)                          |
| `UART_LENGTH_IN_BYTE` | Length read from byte `length_byte_index`        |
| `UART_LENGTH_MODBUS`  | Length derived from the Modbus RTU function code |

**CRC** (`crc_type`)

| Value          | Meaning                                       |
|----------------|-----------------------------------------------|
| `UART_CRC_8`   | 1-byte CRC in the last byte                   |
| `UART_CRC_16`  | 2-byte CRC in the last two bytes (high first) |

## Usage

A complete, compilable slave using a fixed-length, CRC8-protected protocol is in
[`basic_usage.c`](basic_usage.c). The core of it:

```c
static uart_port_t *port;

void slave_init(void)
{
    uart_port_t port_config = {0};
    port_config.mode               = UART_MODE_SLAVE;
    port_config.tx.send_bytes      = send_bytes;   /* your byte sender */
    port_config.rx.byte_life_ms    = 60;
    port_config.rx.max_idle_gap_ms = 5;

    if (uart_add_port(&port, &port_config) != UART_ADD_PORT_OK)
    {
        return;
    }

    uart_device_t device_config = {0};
    device_config.rx_spec.header_type = UART_HEADER_ONE_BYTE;
    device_config.rx_spec.header_byte = 0xAA;
    device_config.rx_spec.length_type = UART_LENGTH_CONST;
    device_config.rx_spec.length      = 4;
    device_config.rx_spec.crc_type    = UART_CRC_8;
    device_config.calc_crc            = calc_crc8;
    device_config.process_frame       = on_frame_received;
    device_config.build_frame         = build_reply;
    device_config.tx_spec.crc_type    = UART_CRC_8;

    uart_add_device(port, &device_config);
}
```

To add a second frame format on the same port (for example Modbus RTU),
configure another `uart_device_t` with `UART_LENGTH_MODBUS` / `UART_CRC_16` and
call `uart_add_device()` again for the same port.

## API summary

| Function           | Description                                              |
|--------------------|----------------------------------------------------------|
| `uart_add_port`    | Register a UART port from a config template              |
| `uart_add_device`  | Attach a device (frame spec + callbacks) to a port       |
| `uart_receive`     | Queue received bytes (call from the RX ISR)              |
| `uart_parse_frame` | Validate/locate a frame in a buffer (used internally; usable standalone) |
| `uart_task`        | Drain RX, parse frames, drive master/slave logic         |
| `uart_tick_1ms`    | 1 ms time base: poll countdowns, byte aging, idle flush  |

### Callbacks

| Type                      | Signature                                     |
|---------------------------|-----------------------------------------------|
| `uart_send_bytes_fn_t`    | `void (const uint8_t *bytes, uint8_t count)`  |
| `uart_calc_crc_fn_t`      | `uint16_t (const uint8_t *bytes, uint8_t count)` |
| `uart_build_frame_fn_t`   | `uint8_t (uint8_t *out_frame)`                 |
| `uart_process_frame_fn_t` | `uint8_t (const uint8_t *frame, uint8_t length)` |

### Result codes

`uart_parse_frame` returns `uart_parse_result_t` — e.g. `UART_PARSE_OK`,
`UART_PARSE_ERROR_HEADER_MISMATCH`, `UART_PARSE_ERROR_BAD_CRC`,
`UART_PARSE_ERROR_NOT_ENOUGH_DATA`. Registration returns `UART_ADD_PORT_OK` /
`UART_ADD_DEVICE_OK` or the matching `*_ERROR_POOL_FULL` / `*_ERROR_NULL_CONFIG`.

## Portability

- C99, no compiler extensions.
- No dynamic allocation; all state in static pools.
- No HAL or platform headers in the module — only `<stdint.h>`, `<stdbool.h>`
  and `<string.h>`.
- Buffer sizes configurable at compile time.

## Files

| File             | Purpose                          |
|------------------|----------------------------------|
| `uart.h`         | Public API, types and config     |
| `uart.c`         | Implementation                   |
| `basic_usage.c`  | Minimal worked slave example     |

## License

Released under the MIT License. Copyright © sd-embedded.
