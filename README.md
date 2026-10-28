# Water On Steroids

An SKSE plugin for Skyrim Special Edition and Anniversary Edition that improves how water is drawn.
It edits no records and ships no meshes or shader code: every change is applied per draw call to the
game's own water meshes and compiled shaders, and the in-game menu can turn it all off on the spot.

## Features

- **Full Shader for Distant Water**: distant (LOD) water is drawn with the same shader as nearby water,
  using the material of the nearest loaded lake or river at the same height.
- **Realistic Distant Water**: water keeps its color and depth at any distance instead of fading into a
  flat sky reflection.
- **Disable River Flow**: hides the seam where flowing rivers meet distant water.
- **Splash Ripples**: bigger ripples when actors jump or fall into water.
- **Clear Underwater**: reduces the underwater blur.
- **Remove Sun Glints**: removes the flashing sun specks on calm water.
- **Water Depth, Transparency and Colors**: shoreline fades, water clarity and custom or tinted colors.
- In-game menu (F3 by default) with live changes, Save and Reset.

## Requirements

- Skyrim SE 1.5.x, AE 1.6.x, or AE 1.7.99 / 1.7.104 (tested in game on 1.6.1170). Skyrim VR and
  versions after 1.7.104 are not supported yet.
- [SKSE64](https://skse.silverlock.org/) (2.3.1 or later for 1.7.104)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

Every engine function the plugin hooks or relies on was checked against the game's code on 1.5.97,
1.6.318, 1.6.1170 and 1.7.104, and its size against the Address Library of every other 1.5, 1.6 and
1.7 version.

Not compatible with Community Shaders: the plugin does not load when it is installed.

ENB: compatible. ENB's own `DisableDistantReflection` setting (`[WATER]` in `enbseries.ini`) also
removes the fade into a flat sky reflection, so with it on, Realistic Distant Water makes no visible
difference.

## Building

Windows, Visual Studio 2022 (C++ and CMake components), Git and vcpkg.

```bat
BuildRelease.bat
```

The script configures and builds the `release` preset and packages the plugin into
`dist/WaterOnSteroids-<version>.zip`. Dependencies (CommonLibSSE-NG, spdlog, Dear ImGui) are fetched at
pinned versions on the first build; the rest come from vcpkg.

## License

GPL-3.0-or-later with the Modding Exception and GPL-3.0 Linking Exception, the same terms as
CommonLibSSE-NG. See [LICENSE](LICENSE), [EXCEPTIONS.md](EXCEPTIONS.md) and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## Credits

- alandtse and contributors: [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)
- Dear ImGui, spdlog, fmt, Microsoft Detours and the other libraries listed in
  [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)
