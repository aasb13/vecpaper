project('vecpaper', 'c')

cc = meson.get_compiler('c')

add_project_arguments('-g3',  language : 'c')

# Deps
dl_dep = cc.find_library('dl', required: false)
rt_dep = cc.find_library('rt', required: false)
wl_client = dependency('wayland-client')
wl_egl = dependency('wayland-egl')
egl = dependency('egl')
libjpeg = dependency('libjpeg')
gles = cc.find_library('GLESv2', required: true)

threads = dependency('threads')

# Wayland protocols
scanner = find_program('wayland-scanner')
scanner_private_code = generator(scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code','@INPUT@','@OUTPUT@'])
scanner_client_header = generator(scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header','@INPUT@','@OUTPUT@'])

protocols_src = [
  scanner_private_code.process('protocols/wlr-layer-shell-unstable-v1.xml'),
  scanner_private_code.process('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
]
protocols_headers = [
  scanner_client_header.process('protocols/wlr-layer-shell-unstable-v1.xml')
]

lib_protocols = static_library('protocols', protocols_src + protocols_headers,
  dependencies: wl_client)

protocols_dep = declare_dependency(link_with: lib_protocols,
  sources: protocols_headers)

# Main executable
executable('vecpaper',
  ['src/main.c', 'src/gl.c', 'src/argparse.c'],
  include_directories: 'include',
  dependencies: [dl_dep, rt_dep, wl_client, wl_egl, egl, gles, threads, protocols_dep, libjpeg],
  install: true
)