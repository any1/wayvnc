uniform mat2 u_proj;

attribute vec2 pos;
attribute vec2 texture;

varying vec2 v_texture;

void main()
{
	v_texture = texture;
	gl_Position = vec4(u_proj * pos, 0, 1);
}
