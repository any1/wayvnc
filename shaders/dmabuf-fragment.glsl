#extension GL_OES_EGL_image_external: require

precision mediump float;

uniform samplerExternalOES u_tex;

varying vec2 v_texture;

void main()
{
	gl_FragColor = texture2D(u_tex, v_texture);
}
