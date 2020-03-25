#extension GL_OES_EGL_image_external: require

precision mediump float;

uniform samplerExternalOES u_tex0;

varying vec2 v_texture;

void main()
{
	float r = float(texture2D(u_tex0, v_texture).rgb != texture2D(u_tex1, v_texture).rgb);
	gl_FragColor = vec4(r);
}
