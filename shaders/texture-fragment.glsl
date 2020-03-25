precision mediump float;

uniform sampler2D u_tex0;

varying vec2 v_texture;

void main()
{
	gl_FragColor = texture2D(u_tex0, v_texture);
}
