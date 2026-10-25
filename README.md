# wxl.unit-outline

A WarcraftXL mod for the recompiled x64 client that draws a retail-style outline around units: green for friendly,
yellow for neutral, red for hostile.

## Options

In game: **Interface → Game → Unit Outlines**.

| Option | Default |
|---|---|
| Enable unit outlines | on |
| Unit under the mouse | on |
| Target | on |
| Nearby units | off |
| Show through walls | off |

The choices are saved to `<mod>\settings.ini` (`[Outline]`). Addons can read and change them with
`WXL_UnitOutline_Get()` and `WXL_UnitOutline_Set(name, on)`.

## Requirements

The `wxl.gx`, `wxl.m2`, `wxl.script` and `wxl.object` services and the `ModelBatchDrawn` event: [WarcraftXL](https://github.com/WarcraftXL) main (ABI 0.1.20) or this fork's `x64-port`.

## Build

Needs a checkout of the WarcraftXL SDK at `../sdk`, or pass `-DWXL_SDK_DIR=...`.
Build with Visual Studio 2022: MSVC 14.50 (VS 2026 Insiders) crashes on the SDK's headers. Since 1.2.0 the mod needs ABI 0.1.20 (WarcraftXL main); older clients refuse it.

```
cmake -S . -B build
cmake --build build --config Release
```

Copy `build\dist\wxl.unit-outline\` into `<client>\bin\Win\Mods\`.

## License

GPL-3.0, see `LICENSE`.
