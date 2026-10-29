shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, specular_schlick_ggx;

#include "res://shaders/common.gdshaderinc"

uniform vec4 shallow : source_color = vec4(0.1, 0.5, 0.6, 0.7);
uniform vec3 deep : source_color = vec3(0.0, 0.1, 0.3);
uniform float amplitude : hint_range(0.0, 2.0) = 0.25;
uniform float speed = 1.5;
uniform int octaves = 2;
uniform bool foam = true;
uniform vec2 flow = vec2(0.1, 0.0);
uniform sampler2D foam_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D normal_tex : hint_normal, repeat_enable;
global uniform float wind_strength;
varying float v_height;

void vertex() {
	float h = wave_at(VERTEX.x, TIME * speed) * amplitude * wind_strength;
	VERTEX.y += h;
	v_height = h;
}

void fragment() {
	vec2 uv = UV + flow * TIME;
	vec3 base = mix(deep, shallow.rgb, clamp(v_height + 0.5, 0.0, 1.0));
	float f = foam ? texture(foam_tex, uv * float(octaves)).r : 0.0;
	ALBEDO = base + vec3(f);
	ALPHA = shallow.a;
	ROUGHNESS = 0.1;
	NORMAL_MAP = texture(normal_tex, uv).rgb;
	SPECULAR = 0.8;
}
