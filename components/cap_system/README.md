# Mosaico system capability

This is the product-owned `cap_system` implementation selected by
`main/idf_component.yml`. It replaces the same-named ESP-Claw component for
this firmware without modifying ESP-Claw sources.

The compatibility boundary consumed by `app_claw` is:

- `cap_system_register_group()`
- `cap_system_time_sync_service_start()`
- the existing user-visible system tool names and JSON contracts

Mosaico device operations and their provider live in this component. Changes
to battery, display, audio, network, power, vibration, Wi-Fi configuration, or
sleep behavior belong here.
