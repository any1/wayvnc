project(
	'wayvnc',
	'c',
	version: '0.1.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

buildtype = get_option('buildtype')
prefix = get_option('prefix')

c_args = [
	'-D_GNU_SOURCE',
]

if buildtype == 'release' or buildtype == 'plain'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libuv = dependency('libuv')
egl = dependency('egl')
glesv2 = dependency('glesv2')
xkbcommon = dependency('xkbcommon')
wayland_client = dependency('wayland-client')

neatvnc_project = subproject(
	'neatvnc',
	required: false,
)

if neatvnc_project.found()
	neatvnc = neatvnc_project.get_variable('neatvnc_dep')
else
	neatvnc = dependency('neatvnc')
endif

inc = include_directories('include')

subdir('protocols')

sources = [
	'src/main.c',
	'src/render.c',
	'src/dmabuf.c',
	'src/strlcpy.c',
	'src/shm.c',
	'src/screencopy.c',
	'src/output.c',
	'src/pointer.c',
	'src/keyboard.c',
	'src/seat.c',
	'src/smooth.c',
	'src/cfg.c',
	'src/intset.c',
]

dependencies = [
	libm,
	pixman,
	libuv,
	egl,
	glesv2,
	wayland_client,
	neatvnc,
	xkbcommon,
	client_protos,
]

config = configuration_data()

config.set('PREFIX', '"' + prefix + '"')

configure_file(
	output: 'config.h',
	configuration: config,
)

install_subdir('shaders', install_dir: 'share/wayvnc')

executable(
	'wayvnc',
	sources,
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)
