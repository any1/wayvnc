precision mediump float;

uniform sampler2D u_tex0;
uniform sampler2D u_tex1;

uniform int u_width;
uniform int u_height;

varying vec2 v_texture;

bool is_pixel_damaged(vec2 pos)
{
	return texture2D(u_tex0, pos).rgb != texture2D(u_tex1, pos).rgb;
}

bool is_region_damaged(vec2 pos)
{
	int x_correction = u_width - (u_width / 32) * 32;
	int y_correction = u_height - (u_height / 32) * 32;

	float x_scaling = float(u_width) / float(u_width - x_correction);
	float y_scaling = float(u_height) / float(u_height - y_correction);

	float x_pixel = 1.0 / float(u_width);
	float y_pixel = 1.0 / float(u_height);

	bool r = false;

	for (int y = -17; y < 17; ++y)
		for (int x = -17; x < 17; ++x) {
			float px = float(x) * x_pixel;
			float py = float(y) * y_pixel;

			float spx = x_scaling * (pos.x + px);
			float spy = y_scaling * (pos.y + py);

			if (is_pixel_damaged(vec2(spx, spy)))
				r = true;
		}

	return r;
}

void main()
{
	float r = float(is_region_damaged(v_texture));
	gl_FragColor = vec4(r);
}
