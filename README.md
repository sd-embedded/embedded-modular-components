# embedded-modular-components

Hardware-agnostic embedded components designed for reusable and configuration-driven firmware.

A collection of small, self-contained C modules for resource-constrained microcontrollers.
Every component is written in plain C99 and contains **no platform code**. The application
connects it to the hardware through function pointers and a periodic tick. The same
component runs unchanged on STM32, PSoC4, STM8S, ESP32 or a PC.

## Design principles

- **Hardware-agnostic** - no HAL, SDK or vendor headers inside components
- **No dynamic memory** - static object pools only, sized at compile time
- **Configurable** - pool and buffer sizes set via `#define`, overridable from the build system
- **Explicit error handling** - public functions report failures through error-code enums
- **Documented** - Doxygen comments on every public function and type
- **Multi-instance** - one component serves many independent objects, e.g. several UART
  ports, each recognising several frame formats, or any number of buttons and LED groups
- **Self-contained** - each component consists of one `.h` and one `.c` file, with no
  dependencies between components

## Components

| Component | Description | Status |
|---|---|---|
| [`button`](components/button) | Button debouncing and event handling: release, long-press and repeat-while-held events | Ready |
| [`led_anim`](components/led_anim) | LED animation engine: on/off, smooth fade and blinking with configurable ramps and dwell times | Ready |
| [`swtimer`](components/swtimer) | Software timers with one-shot and periodic modes, pause/resume and expiry callbacks | Ready |
| [`uart`](components/uart) | UART frame handling for multiple independent UART ports, each supporting several frame formats at once; ring buffer, frame parser, CRC8/CRC16, master and slave modes | Ready |
| `pulse_sequencer` | Timed pulse sequences for outputs | Planned |

## Repository layout

```
embedded-modular-components/
├── components/          # hardware-agnostic components
│   └── <name>/
│       ├── <name>.h     # public API
│       ├── <name>.c     # implementation
│       ├── README.md    # component documentation
│       └── examples/    # minimal usage examples
├── demos/               # platform demos on real hardware (planned)
└── docs/                # diagrams and images
```

## Integration

1. Copy the component folder into your project and add the `.c` file to the build.
2. Adjust the configuration macros in the header, or override them from the build system.
3. Call the component's 1 ms tick function from a hardware timer interrupt.
4. Register your hardware callbacks, e.g. a PWM setter or a UART send function.

The README of each component contains a complete usage example.

## Roadmap

- [ ] Usage examples for all components
- [ ] `pulse_sequencer` component
- [ ] `modbus_rtu` component built on top of `uart`
- [ ] Unit tests (Unity) running on the host
- [ ] Continuous integration (GitHub Actions)
- [ ] Platform demos:
  - [ ] LED blinking with `swtimer` + `led_anim`, one application on **STM32 and PSoC4**
  - [ ] Button-controlled LED modes (`button` + `led_anim` + `swtimer`)
  - [ ] UART demos: Modbus RTU slave, Modbus RTU master, custom protocol with a PC tool

## License

Released under the [MIT License](LICENSE).

## Author

[sd-embedded](https://github.com/sd-embedded) - embedded C engineer (STM32, PSoC4, ESP32/FreeRTOS, STM8S, bare-metal)
