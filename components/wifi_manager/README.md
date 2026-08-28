# Mosaico Wi-Fi manager

This product-owned component is the sole Wi-Fi driver lifecycle owner in the
Mosaico firmware. It provides radio, STA, and scan state machines, structured
multi-subscriber events, operation identifiers, failure details, and
asynchronous radio enable/disable.

All product consumers must resolve this component through the local manifest.
Do not link ESP-Claw's same-named manager into the same firmware, because two
owners would race on ESP event handlers, reconnect timers, and driver state.
