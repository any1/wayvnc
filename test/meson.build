test_inc = include_directories('../include')

test('damage',
	executable('test-damage',
		[
			'test-damage.c',
			'../src/damage.c',
		],
		include_directories: test_inc,
		dependencies: [
			pixman,
			aml,
		]
	)
)
