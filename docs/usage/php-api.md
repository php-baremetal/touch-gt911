---
eyebrow: 'touch-gt911 · PHP API'
lede:    'Constructor, onboard(), point(), points(), maxPoints() and events() of Baremetal\I2c\Driver\Gt911.'
see_also:
  - { href: '../overview.md', meta: 'Overview' }
  - { href: '../recipes/touch-events.md', meta: 'Recipe' }
  - { href: 'package:php-baremetal/ext-input#docs/usage/events.md', meta: 'Events', label: 'Touch events' }
prev: { label: 'GT911 touch controller', href: '../overview.md' }
next: { label: 'React to touches', href: '../recipes/touch-events.md' }
---

# Gt911

<!-- @method name="new Gt911(Bus $bus, int $address = 0x5D, int|string $int = -1, int|string $rst = -1, int $width = 0, int $height = 0, bool $mirror_x = false, bool $mirror_y = false, bool $swap_xy = false, int $poll_ms = 0)" -->

Attaches the chip. `$int` and `$rst` are GPIO numbers, or pins of an I/O expander named as in the board file
(`"exio1"`, with the expander's driver installed); -1 is no pin. With `$rst` the chip is reset and its address
chosen with the INT line (low: `0x5D`, high: `0x14`; INT is driven only when it is a GPIO). Then the product id is
checked and the configuration read (resolution and INT mode); the configuration is never written.

`$width` and `$height` (0 to 4096) are the screen size in the chip's axes, that is before `$swap_xy`; 0 takes the
chip's resolution. The points are scaled from the chip's resolution to that size, then swapped and mirrored (swap
first, then mirror).

`$poll_ms` is used only without an INT line on a GPIO (`$int` -1, or an expander pin): the period, in milliseconds
(5 to 100), at which `events()` reads the chip; 0 is 10 ms.

Throws `\RuntimeException`:

- `Baremetal\I2c\Driver\Gt911: no GT911 answers at 0x5D (product id '...')`, or `(no answer)` when the read fails;
- `Baremetal\I2c\Driver\Gt911: rst 'exio1': ...` (or `int`) when a pin cannot be used, with the reason (for example
  `no installed package provides this pin`);
- when the chip reports no resolution and `$width` or `$height` is 0.

Throws `\ValueError` for a size outside 0 to 4096, a `$poll_ms` other than 0 or 5 to 100, or an address outside `0x08` to `0x77`, and `\Exception`
(`cannot init gt911 at 0x..: ...`) when the device cannot be attached.

<!-- @method name="Gt911::onboard(string $block = 'touch'): Gt911" -->

The controller described by a block of the board, on the bus of the board's `i2c` block. The block has `type`
`"gt911"`, and optionally `int`, `rst` (a GPIO number or an expander pin name), `width`, `height`, `address`,
`mirror-x`, `mirror-y`, `swap-xy` and `poll-ms`. Without `int` the events are polled. Throws `\RuntimeException` when the board has no such block
(`board '<id>' has no 'touch' block`), the type differs (`... of type '<t>', not 'gt911'`), or a pin of the block
cannot be used (`board '<id>': touch.rst: <reason>`).

<!-- @method name="point(): ?array" returns="?array" -->

The first finger still down: `['x' => int, 'y' => int]` in screen coordinates, or `null` with no finger.

<!-- @method name="points(): array" returns="array" -->

The fingers down now, in the order they came down: `[['id' => int, 'x' => int, 'y' => int], ...]`, `[]` with none.
The ids are those of the events.

Before `events()`, `point()` and `points()` read the chip (and a failed read returns no finger). After `events()`
they return what the event service read last, without a transfer on the bus.

<!-- @method name="maxPoints(): int" returns="int" -->

Returns 5: the GT911 reports five fingers.

<!-- @method name="events(): Baremetal\Source" returns="Baremetal\Source" -->

Turns on the events described in [Touch events](package:php-baremetal/ext-input#docs/usage/events.md). With the
INT pin on a GPIO the service follows the line; otherwise (no `$int`, or an expander pin) it reads the chip every
`$poll_ms` milliseconds (see [the overview](../overview.md#int-line-or-polling)). With the line, a chip configured to
signal with INT high (`0x804D` bits 1:0 = `11`) throws `\RuntimeException`. It can be called again: it returns a
new handle on the same source. When the service cannot start it throws `\Error`, and `point()` and `points()` go on
reading the chip as before `events()`.

At each falling edge of INT (or each polling period) the service reads the status with the first point record (and the other records only
when more fingers are down), raises the events, then clears the status:

| Change | Event |
| --- | --- |
| a finger the previous report did not have | `Pressed`, with the lowest free id |
| a finger at another point | `Moved` |
| a finger the report no longer has | `Released` with its last point |

With the line, while a finger is down the service reads again 100 ms after each report. A failed read raises
`Released` for every finger down; polling, the next read is then 500 ms later.
