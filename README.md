# Weather

A weather app for [Tab5 OS](https://github.com/tab5os/tab5os) on the M5Stack Tab5. It shows the current conditions, the next 24 hours and the week ahead, from [Open-Meteo](https://open-meteo.com) (no API key). It also posts a card to the standby screen.

It needs Tab5 OS with app API level 1 and asks for the `net` and `ambient` permissions.

## Build

You need the [Tab5 OS SDK](https://github.com/tab5os/tab5os/blob/main/docs/sdk.md): a tab5os checkout with a firmware build, or the SDK release. The compiler is ESP-IDF v6.1's RISC-V toolchain.

```sh
t5 app-build .            # build/com.t5.example.weather.t5app
t5 run .                  # build, push to a paired Tab5, launch, stream the log
```

Or with CMake directly: `cmake -S . -B build -G Ninja -DT5_SDK=<sdk dir> && cmake --build build`.

To install by hand, copy the `.t5app` to the SD card and tap it in Files.

## History

This app was an example in the tab5os repo (`apps/examples/weather`) until Nov 10, 2026. Its history came with it.

## License

Apache 2.0, see [LICENSE](LICENSE).
