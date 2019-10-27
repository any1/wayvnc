precision mediump float;

uniform sampler2D u_tex;

varying vec2 v_texture;

void main()
{
	gl_FragColor = texture2D(u_tex, v_texture);
}
