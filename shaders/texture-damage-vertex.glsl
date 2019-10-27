attribute vec2 pos;
attribute vec2 texture;
attribute float width;
attribute float height;

varying vec2 v_texture;
varying float v_width;
varying float v_height;

void main() {
	v_texture = texture;
	v_width = width;
	v_height = height;
	gl_Position = vec4(pos, 0, 1);
}
