wayland_scanner = find_program('wayland-scanner')
wayland_client = dependency('wayland-client')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_client = generator(
	wayland_scanner,
	output: '@BASENAME@.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

client_protocols = [
	'wlr-export-dmabuf-unstable-v1.xml',
	'wlr-screencopy-unstable-v1.xml',
	'ext-image-copy-capture-v1.xml',
	'ext-image-capture-source-v1.xml',
	'ext-foreign-toplevel-list-v1.xml',
	'wlr-virtual-pointer-unstable-v1.xml',
	'virtual-keyboard-unstable-v1.xml',
	'xdg-output-unstable-v1.xml',
	'linux-dmabuf-unstable-v1.xml',
	'ext-data-control-v1.xml',
	'wlr-data-control-unstable-v1.xml',
	'wlr-output-management-unstable-v1.xml',
	'wlr-output-power-management-unstable-v1.xml',
	'ext-transient-seat-v1.xml',
]

client_protos_src = []
client_protos_headers = []

foreach xml: client_protocols
	client_protos_src += wayland_scanner_code.process(xml)
	client_protos_headers += wayland_scanner_client.process(xml)
endforeach

lib_client_protos = static_library(
	'client_protos',
	client_protos_src + client_protos_headers,
	dependencies: [
		wayland_client
	]
)

client_protos = declare_dependency(
	link_with: lib_client_protos,
	sources: client_protos_headers,
)
