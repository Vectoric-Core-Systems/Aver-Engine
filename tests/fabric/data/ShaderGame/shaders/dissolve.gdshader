shader_type spatial;
render_mode unshaded, cull_front, shadows_disabled, blend_add;

uniform sampler2D noise : filter_nearest, repeat_disable;
uniform float threshold : hint_range(0.0, 1.0) = 0.5;
uniform vec4 edge_color : source_color = vec4(1.0, 0.4, 0.1, 1.0);
uniform mat3 uv_xform = mat3(1.0);

void fragment() {
	float n = texture(noise, (uv_xform * vec3(UV, 1.0)).xy).r;
	if (n < threshold) {
		discard;
	}
	ALBEDO = edge_color.rgb;
	ALPHA_SCISSOR_THRESHOLD = 0.2;
	EMISSION = edge_color.rgb * 2.0;
}
