# Changelog

## 1.5.0

### Changed

- Distant water takes the water type and height of the cell it lies in. When a terrain block loads,
  each distant (LOD) water triangle gets the water type and exact water height of its cell from
  your plugins' cell records (read once at startup), so distant water matches nearby water of the
  same cell everywhere, also where water types meet, such as the river mouth below Solitude, and at
  the far LOD levels. The few triangles no cell record covers (about 0.05% in vanilla) stay vanilla
  LOD water. Distant water no longer copies nearby water. Only active with Full Shader for Distant
  Water on.
- Distant water now takes its look from its own water type. Each distant piece uses a material built
  from its water type's record, with that type's wave textures and wind, the way the game builds it
  for nearby water, whether or not water of that type is loaded nearby.
- With Disable River Flow on, river water at sea level now moves like the sea. River water types
  have much faster wind than the sea (3 to 5 times at the Solitude harbour, which Update.esm gives
  a river type), made for their flow maps; without the flow map that wind showed as fast waves
  ending at the sea. River water at the worldspace's sea level, nearby and distant, now uses the
  sea's water type. Rivers at other heights are unchanged.
- Removed Splash Ripples and its `bSplashRipples` setting. Splashes are vanilla again; the separate
  plugin Dynamic Splashes covers them.
- Removed Solid Distant Water and its `bSolidDistantWater` setting. Distant water now always keeps
  its depth fades, and nearby water no longer changes as the camera rises.
- Removed the load checks: the plugin no longer refuses to load when Community Shaders is installed,
  on runtimes newer than 1.7.104, or in Skyrim VR. Only SE 1.5.97 and AE up to 1.7.104 are
  supported, and Community Shaders is not supported; with it installed, its own water features may
  conflict.

### Fixed

- Realistic Distant Water no longer shows flickering dark patches on distant water. It used to set
  a frame-wide shader flag that kept water's full shading at any distance, but that flag also kept
  the game's screen-space reflections at full strength far away, where they can only reflect what
  is on screen. It now uses a patched copy of each vanilla water pixel shader, made when the game
  creates the shader and used only while the feature is on: water keeps its color, depth and
  refraction at any distance, and screen-space reflections keep the game's own smooth distance
  fade. With ENB, whose own water shader handles this, water shaders are left alone.
- Distant water pieces no longer stay behind when their terrain block unloads. The game removes only
  the first water shape of an unloading terrain block, so blocks whose distant water is in several
  pieces left the others in the water system, which grew with every block loaded (over 40,000
  objects after a minute of flying over the map with tfc) and lowered the frame rate. All pieces are
  now removed.
- Far distant sea in Solstheim (and a few Tamriel blocks) used a depth plane at height 0 instead of
  the sea level, because its vertex positions were read in the wrong format.

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
