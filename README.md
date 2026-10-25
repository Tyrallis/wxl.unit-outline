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

The `wxl.gx`, `wxl.script` and `wxl.object` runtime services. `wxl.gx` is on the `x64-port` branch of
[Tyrallis/client](https://github.com/Tyrallis/client/tree/x64-port), [Tyrallis/core](https://github.com/Tyrallis/core/tree/x64-port) and
[Tyrallis/sdk](https://github.com/Tyrallis/sdk/tree/x64-port).

## Build

Needs a checkout of the WarcraftXL SDK (`x64-port` branch) at `../sdk`, or pass `-DWXL_SDK_DIR=...`.
Build with Visual Studio 2022: the 2026 Insiders compiler (MSVC 14.50) crashes on the SDK's headers. Since 1.1.0 the mod needs the client and SDK of `x64-port` at WarcraftXL ABI 0.1.15; older clients refuse it.

```
cmake -S . -B build
cmake --build build --config Release
```

Copy `build\dist\wxl.unit-outline\` into `<client>\bin\Win\Mods\`.

## License

GPL-3.0, see `LICENSE`.
