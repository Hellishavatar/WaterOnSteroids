# Changelog

## 1.3.0

### Added

- Solid Distant Water (BETA, `bSolidDistantWater`, on by default): distant water is drawn solid and
  reflective, without depth fades and with a tenth of the fog distance, so the coarse distant
  terrain under it no longer makes it read deeper and darker than nearby water at the edge of the
  loaded cells. Nearby water blends smoothly to the same look as the camera rises from 512 to 2048
  units above it, and is unchanged closer to the surface. Needs Full Shader for Distant Water.

## 1.2.1

### Changed

- The menu uses a true dark theme with a muted gold accent instead of cyan, and softer text colors
  for lower contrast.

## 1.2.0

### Added

- Mixed-height distant water is split per height. Distant (LOD) water pieces that contain water at
  several heights, such as a lake above the sea or a river stepping down in one LOD block, are split
  into one flat piece per height when their terrain block loads, using the game's own shape factory.
  Each piece then gets a depth plane at its own height and its own water type match, so the lower
  water no longer looks deeper and darker than its neighbours. Mixed pieces occur at LOD levels 8,
  16 and 32.

### Changed

- The menu uses a neutral dark theme without borders.

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
