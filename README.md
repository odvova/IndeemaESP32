# Smart Device Project
This is an ESP32 project that demonstrates custom components and Kconfig.

## Wi-Fi component (WS2812 strategy)

The project includes a reusable `wifi_component` in `components/wifi_component`.

LED states:
- White solid: Wi-Fi off / not started
- Yellow solid: STA connecting
- Red solid: STA connection error (until next retry)
- Green blink: STA has IP, internet check pending/failing
- Green solid: internet access check passed
- Blue blink: SoftAP started, no clients
- Blue solid: SoftAP has at least one client

Configure credentials in `menuconfig`:
- `Wi-Fi Component Configuration -> Wi-Fi STA SSID`
- `Wi-Fi Component Configuration -> Wi-Fi STA Password`
- `Wi-Fi Component Configuration -> Wi-Fi AP SSID`
- `Wi-Fi Component Configuration -> Wi-Fi AP Password`