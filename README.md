# touch-gt911

A driver for the GT911 capacitive touch controller (Goodix) on I2C: up to five fingers, each with an id kept from
its `Pressed` to its `Released`. It registers into ext-i2c's `i2c_drivers` registry, so a firmware built with it
has the class `Baremetal\I2c\Driver\Gt911`, which implements `Baremetal\Input\Touch` of ext-input.

## Install

```bash
composer require php-baremetal/touch-gt911
```

ext-i2c, ext-input, ext-gpio and bm-common come with it. On a board whose reset line is on an I/O expander (the
Waveshare ESP32-S3-Touch-LCD-5B), also require the expander's driver (`php-baremetal/io-ch422g`).

## Example

```php
<?php
use Baremetal\Events;
use Baremetal\I2c\Driver\Gt911;
use Baremetal\Input\{Pressed, Released};

$t = Gt911::onboard();         // the board's 'touch' block, on the bus of its 'i2c' block
Events::listen(Pressed::class,  fn(Pressed $e)  => printf("finger %d down at %d,%d\n", $e->id, $e->x, $e->y));
Events::listen(Released::class, fn(Released $e) => printf("finger %d up\n", $e->id));
$t->events();
```

Without a touch block, give the bus and GPIO numbers: `new Gt911(new Bus(sda: 8, scl: 9), int: 4, rst: 5)`; an
expander pin (`rst: 'exio1'`) also needs the board's `expander` block. Use the `event-driven` runtime
for events; `points()` also works without them. Without `int` the events are polled.

## How it works

The chip has 16-bit register addresses: the driver writes the address, then reads in a second transfer. At each
falling edge of INT (or, when INT is not wired, every `poll-ms` milliseconds, 10 by default) the
event service (on the executor task, which owns the bus) reads the status byte and the
point records of the fingers down (the first one with the status, the others only when there are more), matches the
chip's track ids with the fingers it knows, raises the events and clears the status. A new finger takes the lowest free id. Points are scaled from the chip's resolution to the screen and turned
as the display is (`swap-xy`, `mirror-x`, `mirror-y`). The chip's configuration, INT mode included, is read and never
written.

## Documentation

- [Overview](docs/overview.md)
- [Gt911](docs/usage/php-api.md)
- [React to touches](docs/recipes/touch-events.md)

## License

MIT.
