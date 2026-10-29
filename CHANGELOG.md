# Changelog

## 1.1.0

### Changed

- Distant water matching is now based on water type instead of height. At startup the plugin reads
  the water height, water type and water flag of every exterior cell from the active plugins, in
  load order (about 0.3 s for the base game with all DLC and Creation Club content). Each distant
  piece then uses the material of the nearest loaded water of its own type, at any height; water at
  the same height is used only when that type is not loaded nearby.
- Distant pieces that span several water heights can now be matched by type. Their depth plane is
  unchanged.
- The live water log reports each distant piece's water type and whether it matched by type or by
  height.

### Fixed

- Distant water lost its matched material when no nearby water had exactly the same height, and
  fell back to the LOD water material. For example, the river below Riverwood showed the LOD water
  look (bright blue with Water for ENB) once its nearest cell unloaded.
- Distant water could take the material of a different water body at the same height, such as a
  lake one unit above sea level.

### Dependencies

- zlib 1.3.1 (compiled in, for compressed plugin records). Its notice is in THIRD_PARTY_NOTICES.txt.

## 1.0.2

### Added

- Support for Skyrim 1.7.99 and 1.7.104. The plugin's code is unchanged; it now loads on every
  runtime up to 1.7.104 (it refused 1.7+ before). Its hooks and the water shaders were checked
  against the 1.7.104 executable: the water shader and render functions are identical to 1.6.1170,
  Splash Ripples finds its call site, and Clear Underwater's function reads the same data (1.7 added
  a new check to it). Not yet tested in game on 1.7.

## 1.0.1

### Fixed

- Distant water with water mods that give a worldspace its own LOD water type, such as Water for
  ENB: matched distant water now uses the wave textures of the nearby water it copies, not the LOD
  water's. Before, nearby wave settings ran on textures made for different ones, so distant waves
  looked stiff and showed a seam against nearby water.

## 1.0.0

- Initial release.
