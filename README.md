# vecpaper

A lightweight, high-performance live wallpaper renderer for Wayland compositors (especially Hyprland) using OpenGL ES 2.0, wlr-layer-shell, and libjpeg-turbo-style in-memory caching.
Inspired by shadertoy and mpvpaper, vecpaper lets you run real-time fragment shaders as animated desktop backgrounds with optional frame caching to reduce GPU load during loops.

## Dependencies
- [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)
- [libegl]
- [gles2]

## Building 
### Building Requirements:

- ninja
- meson

### Clone | Build | Install:
```
git clone --single-branch https://github.com/aasb13/vecpaper
cd vecpaper
meson setup build --prefix=/usr/local
ninja -C build
ninja -C build install
```

## Hyprland
For now, cursor position is working only under hyprland and is passed under vec2 mouse uniform

## Usage
```
vecpaper --shader path/to/shader [options]
```
Use one of the example shaders:
```
vecpaper -s examples/voronoi_on_sphere.glsl
```
Using caching to loop the shader:
```
vecpaper -s examples/voronoi_on_sphere.glsl --cache 10
```
## Credits
- Mpvpaper for the base code: https://github.com/GhostNaN/mpvpaper