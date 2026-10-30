# Weather location

IP geolocation is an estimate of the public network exit. It cannot reliably
identify the device's city. Tap the city in Weather to search for and select a
city, or explicitly return to network location. A selected city is stored
separately from the disposable forecast cache and takes priority after reboot.

Location failures keep `location_stale` and the error visible even when a
forecast for the previous coordinates succeeds. A change of location invalidates
the previous forecast. A delayed HTTP response cannot replace a newer selection.
Search runs on the existing worker and exposes pending, empty and failed states.
City/revision and connection-state changes commit atomically. Notifications may
overlap across tasks; UI staging discards older snapshot sequence numbers. Empty
forecast days clear previous row values, including while a new city is loading.

The default explicit search endpoint is the [Open-Meteo geocoding API](https://open-meteo.com/en/docs/geocoding-api),
using WGS84 coordinates and GeoNames location data. It receives only the search
text. `CONFIG_WEATHER_CITY_SEARCH_URL` can select a compatible licensed endpoint
or product backend; the default public endpoint has separate commercial usage
terms. Forecasts continue to use MET Norway. Day grouping currently follows the
device timezone; selecting a city does not change the system clock/timezone.
