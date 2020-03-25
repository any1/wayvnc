precision mediump float;

uniform sampler2D u_tex0;
uniform sampler2D u_tex1;

varying vec2 v_texture;

void main()
{
	float r = float(texture2D(u_tex0, v_texture).rgb != texture2D(u_tex1, v_texture).rgb);
	gl_FragColor = vec4(r);
}
