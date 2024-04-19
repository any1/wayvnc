project(
	'wayvnc',
	'c',
	version: '0.9-dev',
	license: 'ISC',
	default_options: [
		'c_std=gnu11',
		'warning_level=2',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()
prefix = get_option('prefix')

c_args = [
	'-D_GNU_SOURCE',
	'-DAML_UNSTABLE_API=1',

	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
]

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"v@0@-@1@ (@2@)"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

add_project_arguments('-DPROJECT_VERSION=@0@'.format(version), language: 'c')

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)
librt = cc.find_library('rt', required: false)
libpam = cc.find_library('pam', required: get_option('pam'))

pixman = dependency('pixman-1')
gbm = dependency('gbm', required: get_option('screencopy-dmabuf'))
drm = dependency('libdrm')
xkbcommon = dependency('xkbcommon', version: '>=1.0.0')
wayland_client = dependency('wayland-client')
jansson = dependency('jansson')

aml_version = ['>=0.3.0', '<0.4.0']
neatvnc_version = ['>=0.9', '<0.10.0']

neatvnc_project = subproject(
	'neatvnc',
	required: false,
	version: neatvnc_version,
)

aml_project = subproject('aml', required: false, version: aml_version)
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml', version: aml_version)
endif

if neatvnc_project.found()
	neatvnc = neatvnc_project.get_variable('neatvnc_dep')
else
	neatvnc = dependency('neatvnc', version: neatvnc_version)
endif

inc = include_directories('include')

subdir('protocols')

sources = [
	'src/main.c',
	'src/strlcpy.c',
	'src/shm.c',
	'src/screencopy.c',
	'src/data-control.c',
	'src/output.c',
	'src/output-management.c',
	'src/pointer.c',
	'src/keyboard.c',
	'src/seat.c',
	'src/smooth.c',
	'src/cfg.c',
	'src/intset.c',
	'src/buffer.c',
	'src/pixels.c',
	'src/transform-util.c',
	'src/util.c',
	'src/json-ipc.c',
	'src/ctl-server.c',
	'src/ctl-commands.c',
	'src/option-parser.c',
        'src/table-printer.c',
]

dependencies = [
	libm,
	librt,
	pixman,
	aml,
	gbm,
	drm,
	wayland_client,
	neatvnc,
	xkbcommon,
	client_protos,
	jansson,
]

ctlsources = [
	'src/wayvncctl.c',
	'src/util.c',
	'src/json-ipc.c',
	'src/ctl-client.c',
	'src/ctl-commands.c',
	'src/strlcpy.c',
	'src/option-parser.c',
        'src/table-printer.c',
]

ctldependencies = [
	jansson,
]

config = configuration_data()

config.set('PREFIX', '"' + prefix + '"')

if host_system == 'linux' and get_option('systemtap') and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

if cc.has_header('linux/dma-heap.h') and cc.has_header('linux/dma-buf.h')
	config.set('HAVE_LINUX_DMA_HEAP', true)
endif

if cc.has_function('memfd_create')
	config.set('HAVE_MEMFD', true)
	config.set('HAVE_MEMFD_CREATE', true)
elif cc.has_function('SYS_memfd_create', prefix : '#include <sys/syscall.h>')
	config.set('HAVE_MEMFD', true)
endif

if gbm.found() and not get_option('screencopy-dmabuf').disabled()
	config.set('ENABLE_SCREENCOPY_DMABUF', true)
endif

if libpam.found()
	dependencies += libpam
	sources += 'src/pam_auth.c'
	config.set('ENABLE_PAM', true)
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

executable(
	'wayvnc',
	sources,
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)

executable(
	'wayvncctl',
	ctlsources,
	dependencies: ctldependencies,
	include_directories: inc,
	install: true,
)

scdoc = dependency('scdoc', native: true, required: get_option('man-pages'))
if scdoc.found()
	scdoc_prog = find_program(scdoc.get_pkgconfig_variable('scdoc'), native: true)
	sh = find_program('sh', native: true)
	mandir = get_option('mandir')
	manpages = {
		'wayvnc.scd': 'wayvnc.1',
		'wayvncctl.scd': 'wayvncctl.1',
	}

	foreach input, output : manpages
		custom_target(
			output,
			input: input,
			output: output,
			command: [
				sh, '-c', '@0@ <@INPUT@ >@1@'.format(scdoc_prog.path(), output)
			],
			install: true,
			install_dir: '@0@/man1'.format(mandir)
		)
	endforeach
endif

if get_option('tests')
	subdir('test')
endif
