# Changelog

## 1.0.0

- First release.
- Added: the class Baremetal\I2c\Driver\Gt911 (registered into ext-i2c's i2c_drivers), implementing Baremetal\Input\Touch: up to five fingers with an id each, point(), points(), maxPoints() and Pressed, Moved and Released events (ext-input 1.1).
- Added: onboard() from the board's touch block; the INT and reset pins are GPIO numbers or expander pins named in the board file ("exio1").
- Added: points scaled from the chip's resolution to the screen size, then swapped and mirrored as the display does; the chip's configuration is read, never written.
- Added: events follow the INT line when `int` is on a GPIO. Without `int` on a GPIO (no `int`, or an expander pin), the event service reads the chip on a timer, every `$poll_ms` milliseconds (constructor argument, board key `poll-ms`, 5 to 100; 0 or absent: 10), with the same reads, events and ids. After a failed read the next look is 500 ms later, and a chip that stops answering is logged once (again only after it answered and failed anew). The timer runs only once `events()` is on. Needs php-baremetal/i2c-hal 1.3 (a service without a line).
