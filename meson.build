project(
	'wayvnc',
	'c',
	version: '0.0.0',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
	],
)

add_project_arguments([
	'-D_GNU_SOURCE',
	'-fvisibility=hidden',
], language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)

pixman = dependency('pixman-1')
libuv = dependency('libuv')
egl = dependency('egl')
glesv2 = dependency('glesv2')
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
]

dependencies = [
	libm,
	pixman,
	libuv,
	egl,
	glesv2,
	wayland_client,
	neatvnc,
	client_protos,
]

executable(
	'wayvnc',
	sources,
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)
