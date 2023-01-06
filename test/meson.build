test('table-printer', executable('table-printer',
	[
		'table-printer-test.c',
		'../src/table-printer.c',
	],
	include_directories: inc,
	dependencies: [ ],
))
test('option-parser', executable('option-parser',
	[
		'option-parser-test.c',
		'../src/option-parser.c',
		'../src/table-printer.c',
		'../src/strlcpy.c',
	],
	include_directories: inc,
	dependencies: [ ],
))
