precision mediump float;

uniform sampler2D u_tex0;
uniform sampler2D u_tex1;

uniform int u_width;
uniform int u_height;

varying vec2 v_texture;

bool is_damaged(vec2 pos)
{
	return texture2D(u_tex0, pos).rgb != texture2D(u_tex1, pos).rgb;
}

void main()
{
	float r = float(is_damaged(v_texture));
	gl_FragColor = vec4(r);
}
