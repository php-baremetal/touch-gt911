---
eyebrow: 'Package · Driver'
lede:    'The GT911 touch controller as a PHP class: up to five fingers, each with an id from its Pressed to its Released, on demand or as touch events, from the INT line or polled when it is not wired. Registers into ext-i2c and implements the Touch interface of ext-input.'
see_also:
  - { href: './usage/php-api.md', meta: 'PHP API' }
  - { href: './recipes/touch-events.md', meta: 'Recipe' }
  - { href: 'package:php-baremetal/ext-input', meta: 'Extension', label: 'ext-input' }
  - { href: 'package:php-baremetal/ext-i2c', meta: 'Extension', label: 'ext-i2c' }
prev: { label: 'No previous page', href: '#' }
next: { label: 'Gt911', href: './usage/php-api.md' }
---

# GT911 touch controller

touch-gt911 adds the class `Baremetal\I2c\Driver\Gt911`. `points()` returns the fingers down now; `events()` turns
on `Pressed`, `Moved` and `Released` events served from the chip's INT line (or read on a timer when INT is not
wired), each with the finger's `$id`.

| Property | Value |
| --- | --- |
| Class | `Baremetal\I2c\Driver\Gt911`, extends `Baremetal\I2c\Device` |
| Capability | `Baremetal\Input\Touch` (ext-input) |
| Addresses, clock | `0x5D` (default) or `0x14`, 400 kHz |
| Fingers | five (`maxPoints()` is 5, ids 0 to 4) |
| Gestures | none: the GT911 reports points only |
| Board block | `"touch": { "type": "gt911", ... }` |

## Requirements

| Requires | Why |
| --- | --- |
| `php-baremetal/ext-i2c` 1.1.1 or later | The `Bus` and `Device` classes, the `i2c_drivers` registry, and the interrupt service. |
| `php-baremetal/i2c-hal` 1.3 or later | The event service without a line, for polling (provided by `php-baremetal/esp32-i2c-hal` 1.2). |
| `php-baremetal/ext-input` 1.1 or later | The `Touch` interface, the event classes and their `$id`. |
| `php-baremetal/ext-gpio` 1.2 or later | Pins by name (`bm_pin.h`): the reset and INT lines can be GPIOs or pins of an I/O expander (`"exio1"`). |

On a board whose reset line is on an expander (the Waveshare ESP32-S3-Touch-LCD-5B: `"rst": "exio1"`), also
require the expander's driver (`php-baremetal/io-ch422g`).

## Finger ids

The chip gives each finger a track id of its own. The driver turns it into the `$id` of the events: the lowest id
free when the finger comes down, kept until it is lifted. With fingers 0, 1 and 2 down, lifting 1 and putting a
finger down again gives it id 1. `points()` lists the fingers in the order they came down, and `point()` is the
first of them.

In one report, the `Released` of the fingers gone come first, then the `Moved` of those that moved, then the
`Pressed` of the new ones.

## Coordinates

The chip reports points in its own resolution, which it holds in its configuration (`0x8048`, `0x804A`). The driver
scales them to `$width` x `$height` (the screen in the chip's axes; 0 or absent: the chip's resolution, so no
scaling), keeps them inside, then applies `swap_xy`, `mirror_x` and `mirror_y` as the display does (swap first, then
mirror against the screen after the swap). The result is `0 <= x < width`, `0 <= y < height` of the screen as drawn.

On the Waveshare ESP32-S3-Touch-LCD-5B the chip reports 1024 x 600, the size of the panel. Held in landscape with
the USB cable on the right, x grows from left to right along the long side and y from top to bottom: the board
file needs no `swap-xy`, `mirror-x` or `mirror-y`. The chip's configuration has its X2Y bit set (bit 3 of `0x804D`),
but the coordinates it reports already include that swap, so the driver must not swap again. Checked with the
example `06-display/touch-fingers`: with no swap or mirror, every finger's circle is drawn under the finger.

On the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 the chip reports 720 x 1280, the panel's native portrait, while the
display block turns the picture to 1280 x 720 with `rotation` 90. The board file gives `width` 720 and `height`
1280 (the chip's axes) with `swap-xy` and `mirror-y`, so a view point `(x, y)` comes from the chip's
`(719 - y, x)`, the same turn the display applies to its framebuffer. With `rotation` 270 the touch needs
`swap-xy` and `mirror-x` instead.

## INT line or polling

With `int` on a GPIO, the event service follows the INT line (below). Without it (no `int`, or `int` on an expander
pin, whose edges the service cannot follow), `events()` starts the same service on a timer: every `poll_ms`
milliseconds it reads the status with the first point record, exactly as after an edge, so the events, the ids and
`points()` are the same. A report not ready yet (status bit 7 clear) changes nothing, so a finger held still is not
released between two reports. `poll_ms` is the constructor's `$poll_ms` or the board key `poll-ms` (5 to 100); 0 or
absent is 10 ms, so a touch is seen within 10 ms, under a frame of a 55-60 Hz display (16-18 ms). The chip writes a
report every 5 + N ms (N in bits 3:0 of its Refresh_Rate register, `0x8056`): a period below that only adds reads
that find no new report.

The cost is two transfers per period on the executor task, with or without a finger (the register address written,
then nine bytes read: 13 bytes on the wire with the device address bytes), about 0.3 ms of bus time at 400 kHz, so
about 3 % of the bus at a 10 ms period. Each ready report (a finger down, or the last one lifted) also costs the
write that clears the status (a third transfer, 4 bytes on the wire) and, with more than one finger, the read of the
other records, as with the line. The timer starts with `events()`; before it, nothing is read in the background and
`point()` / `points()` read the chip themselves. After a failed read the next one is 500 ms later, and a chip that
stops answering is logged once, not at every look; once it answers again, a new failure is logged again. The timer
runs until the program ends, like the interrupt service.

On the Waveshare ESP32-P4-WIFI6-Touch-LCD-5 the INT line of the GT911 is not wired (its reset is, on GPIO 23), so
its board file has no `int` and the events are polled.

## INT line and configuration

The driver reads the chip's configuration and never writes it: the resolution and the INT mode (`0x804D`, bits 1:0)
stay as the panel maker set them. The event service of ext-i2c follows the falling edge of INT. In the pulse modes
(`00` rising, `01` falling) every report is a short pulse, which has a falling edge, and in the low-level mode (`10`)
the line falls when a report is ready: these work. A chip configured for the high-level mode (`11`) cannot be
followed, and `events()` throws `\RuntimeException`.

While a finger is down the service also reads the chip 100 ms after each report without waiting for an edge, so a
release whose pulse was missed still ends the touch. A failed read releases every finger down at its last point.

## Reset and address

With a reset pin, the constructor resets the chip and chooses its address with the INT line: reset
low, INT low for `0x5D` or high for `0x14`, 100 ms, reset high, 200 ms, then INT back to
an input. INT is driven only when it is a GPIO; without it the chip takes the address its own INT level gives
(`0x5D` on the Waveshare ESP32-P4-WIFI6-Touch-LCD-5). Without a reset pin the chip keeps the address it chose at
power-on.
Then the driver checks the product id (`0x8140`, `"911"`).

## Notes

- One shared bus: the event service runs on the executor task, the task that owns the I2C bus, so it does not
  collide with other devices on the same bus.
- The pins are driven only by the constructor, on the PHP task; the event service never touches them, so an
  expander pin is safe as the reset line.
- Constructing the class again for the same bus and address returns a handle to the same device; the first
  construction decides the pins, size and orientation.
