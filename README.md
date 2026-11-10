# Weather

A weather app for [Tab5 OS](https://github.com/tab5os/tab5os) on the M5Stack Tab5. It shows the current conditions, the next 24 hours and the week ahead, from [Open-Meteo](https://open-meteo.com) (no API key). It also posts a card to the standby screen.

It needs Tab5 OS with app API level 1 and asks for the `net` and `ambient` permissions.

## Build

This repo is set up like the [app template](https://github.com/tab5os/app-template). You need ESP-IDF v6.1 (or just its RISC-V toolchain), CMake 3.22+, Ninja, Python 3.11+ and curl.

```sh
./build.sh                # build/com.t5.example.weather.t5app
```

The first run downloads the Tab5 OS SDK version in `tab5os-sdk.txt` into `.t5sdk/` and checks its checksum. Set `T5_SDK` to use an SDK you already have.

To install, copy the `.t5app` to the SD card and tap it in Files. You can also get it from this repo's [releases](https://github.com/tab5os/weather/releases), or from the latest CI run's artifacts.

## CI and releases

Every push builds the `.t5app` in CI and keeps it as an artifact. To release, set `version` in `manifest.toml`, commit, and push a matching tag (`v2.0.0` for `version = "2.0.0"`). CI then publishes a GitHub release with the `.t5app` attached.

## History

This app was an example in the tab5os repo (`apps/examples/weather`) until Nov 10, 2026. Its history came with it.

## License

Apache 2.0, see [LICENSE](LICENSE).
