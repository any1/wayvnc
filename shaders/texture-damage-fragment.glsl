precision mediump float;

uniform sampler2D tex0;
uniform sampler2D tex1;

varying vec2 v_texture;
varying float v_width;
varying float v_height;

float get_pixel_damage(vec2 coord)
{
	vec3 diff = texture2D(tex0, coord).rgb - texture2D(tex1, coord).rgb;

	vec3 absdiff = ceil(abs(diff));

	return clamp(absdiff.r + absdiff.g + absdiff.b, 0.0, 1.0);
}

float get_damage(vec2 coord)
{
	float x_off = 2.0 / v_width;
	float y_off = 2.0 / v_height;

	float ix = v_width * (1.0 + coord.x) * 2.0;
	float iy = v_height * (1.0 + coord.y) * 2.0;

	float x_start = mod(ix, 32.0) / v_width / 2.0;
	float y_start = mod(iy, 32.0) / v_height / 2.0;

	float color = 0.0;
	for (float y = 0.0; y < 32.0; y += 1.0)
		for (float x = 0.0; x < 32.0; x += 1.0) {
			color += get_pixel_damage(vec2(coord.x - x_start + x * x_off,
						       coord.y - y_start + y * y_off));
		}

	return clamp(color, 0.0, 1.0);
}

void main()
{
	float color = get_damage(v_texture);
	gl_FragColor = vec4(color, color, color, 1.0);
}
