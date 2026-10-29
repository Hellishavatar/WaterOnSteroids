# Water On Steroids

An SKSE plugin for Skyrim Special Edition and Anniversary Edition that extends the engine's water
rendering. It modifies no game records and ships no meshes or shaders: every change is applied per
draw call to the game's own water geometry and compiled shader techniques, and every feature can be
toggled at runtime.

## Features

### Distant water

- **Full Shader for Distant Water**: renders distant (LOD) water with the full water technique used
  for nearby water (reflections, refraction and depth) instead of the reduced LOD technique. Each
  distant piece uses the material (settings and wave textures) of the nearest loaded water of its own
  water type, and its depth plane is set from its measured surface height. Per-cell water types are
  read from the active plugins once at startup; when a type is not loaded nearby, water at the same
  height is used instead. Pieces that contain water at several heights are split into one flat piece
  per height when their terrain block loads.
- **Realistic Distant Water**: keeps full water shading beyond the engine's distance fade, which
  otherwise blends water into a flat sky reflection past 8192 units.
- **Disable River Flow**: draws flow-map rivers without their flow map, removing the seam where they
  meet distant water.

### Nearby water

- **Splash Ripples**: larger ripples when actors jump or fall into water.
- **Clear Underwater**: reduces the underwater depth-of-field blur.
- **Remove Sun Glints**: removes sun sparkle on calm water.
- **Water Depth**: overrides the depth properties that fade reflections, refraction, waves and
  specular lighting toward the shore.
- **Water Transparency**: scales the water fog distance.
- **Water Colors**: replaces or tints the shallow, deep and reflection colors.

### Configuration

- In-game menu (F3 by default) with live preview, Save to the INI and Reset.
- Optional live water log for diagnosing water rendering.

## Requirements

| Component | Version |
| --- | --- |
| Skyrim | SE 1.5.x, AE 1.6.x, AE 1.7.99 / 1.7.104 (tested in game on 1.6.1170 and 1.7.104) |
| [SKSE64](https://skse.silverlock.org/) | The build matching the game version (2.3.1 or later for 1.7.104) |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | Current release |

Skyrim VR and versions after 1.7.104 are not supported. Every engine function the plugin hooks or
relies on was checked against the game's code on 1.5.97, 1.6.318, 1.6.1170 and 1.7.104, and its size
against the Address Library of every other 1.5, 1.6 and 1.7 version.

## Compatibility

- **Community Shaders**: not supported. The plugin detects `CommunityShaders.dll` and does not load.
- **ENB**: compatible. ENB's `DisableDistantReflection` setting (`[WATER]` in `enbseries.ini`) also
  removes the distance fade, so with it enabled Realistic Distant Water has no visible effect.
- **Water and worldspace mods**: water heights and types are taken from the load order, so mods that
  change water types (including custom LOD water types, as in Water for ENB) and new worldspaces are
  handled without patches.

## Technical overview

- `BSWaterShader` `SetupTechnique`, `SetupMaterial` and `SetupGeometry` (vtable slots 2, 4 and 6):
  technique upgrade for distant water, flow-map removal, per-draw material overrides, distant-water
  matching and depth plane.
- `ImageSpaceEffectDepthOfField::UpdateParams`: underwater blur.
- `BGSTerrainChunk` constructor: splits mixed-height LOD water shapes per height as each terrain block
  loads.
- The actor water-entry splash: ripple size.
- The per-frame shader constants update and the accumulator pass-range draw: distance fade. Both are
  located at runtime from CommonLib-named functions rather than Address Library IDs.
- Cell water map: at startup, the plugin reads each active plugin's worldspace cells (water height,
  water type and water flag) with its own record reader, in load order.

## Building

Windows, Visual Studio 2022 (C++ and CMake components), Git and vcpkg.

```bat
BuildRelease.bat
```

The script configures and builds the `release` preset and packages the plugin into
`dist/WaterOnSteroids-<version>.zip`. CommonLibSSE-NG, spdlog and Dear ImGui are fetched at pinned
versions on the first build; the remaining dependencies come from vcpkg.

## License

GPL-3.0-or-later with the Modding Exception and GPL-3.0 Linking Exception, the same terms as
CommonLibSSE-NG. See [LICENSE](LICENSE), [EXCEPTIONS.md](EXCEPTIONS.md) and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## Credits

- alandtse and contributors: [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)
- Dear ImGui, spdlog, {fmt}, Microsoft Detours, zlib and the other libraries listed in
  [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)
