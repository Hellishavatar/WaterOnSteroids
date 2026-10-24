# Changelog

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
