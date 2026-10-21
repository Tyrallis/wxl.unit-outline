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

The `wxl.gx`, `wxl.script` and `wxl.object` runtime services. `wxl.gx` is on the `mod-support` branch of
[Tyrallis/client](https://github.com/Tyrallis/client), [Tyrallis/core](https://github.com/Tyrallis/core) and
[Tyrallis/sdk](https://github.com/Tyrallis/sdk).

## Build

Needs a checkout of the WarcraftXL SDK (`mod-support` branch) at `../sdk`, or pass `-DWXL_SDK_DIR=...`.

```
cmake -S . -B build
cmake --build build --config Release
```

Copy `build\dist\wxl.unit-outline\` into `<client>\bin\Win\Mods\`.

## License

GPL-3.0, see `LICENSE`.
