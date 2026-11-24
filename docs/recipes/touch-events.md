---
eyebrow: 'touch-gt911 · Recipe'
lede:    'Print the fingers of a GT911 as they come down, move and lift, in an event-driven project.'
see_also:
  - { href: '../usage/php-api.md', meta: 'PHP API' }
  - { href: 'package:php-baremetal/ext-input#docs/usage/events.md', meta: 'Events', label: 'Touch events' }
prev: { label: 'Gt911', href: '../usage/php-api.md' }
next: { label: 'No next page', href: '#' }
---

# React to touches

On a board with a `touch` block of type `gt911` (the Waveshare ESP32-S3-Touch-LCD-5B, or the Waveshare
ESP32-P4-WIFI6-Touch-LCD-5, where the events are polled because INT is not wired), `onboard()` takes the pins and
the size from the board file. The listeners print each event with its finger id; a timer prints `points()` once
a second.

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Events;
use Baremetal\I2c\Driver\Gt911;
use Baremetal\Input\{Pressed, Moved, Released, TouchEvent};

$t = Gt911::onboard();
echo 'maxPoints ', $t->maxPoints(), "\n";
$t->events();
foreach ([Pressed::class, Moved::class, Released::class] as $c) {
    Events::listen($c, function (TouchEvent $e) {
        printf("%d %s id=%d %d,%d\n", sys_micros(), (new ReflectionClass($e))->getShortName(), $e->id, $e->x, $e->y);
    });
}
final class Second extends \Baremetal\TimerEvent {}
every(1000, Second::class);
Events::listen(Second::class, function () use ($t) { echo 'points ', json_encode($t->points()), "\n"; });
```
<!-- @endcode-block -->

Require `php-baremetal/touch-gt911`, `php-baremetal/ext-sys` (for `sys_micros()`) and, on the Waveshare
ESP32-S3-Touch-LCD-5B, `php-baremetal/io-ch422g` (the reset line is on the expander), and use the `event-driven`
runtime.

Without a touch block, give the bus and GPIO numbers: `new Gt911(new Bus(sda: 8, scl: 9), int: 4, rst: 5)`. An
expander pin (`rst: 'exio1'`) also needs the board's `expander` block and the expander's driver. With
a wrong orientation the points land mirrored or swapped: set `mirror_x`, `mirror_y` or `swap_xy` (`mirror-x`,
`mirror-y`, `swap-xy` in the board block) until a touch in a corner reports that corner.
