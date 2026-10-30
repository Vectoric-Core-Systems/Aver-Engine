shader_type spatial;
render_mode diffuse_toon, specular_toon, vertex_lighting;

uniform vec3 color : source_color = vec3(0.9, 0.2, 0.2);
uniform float bands = 3.0;
instance uniform float tint_amount = 1.0;

void fragment() {
	ALBEDO = color * tint_amount;
	RIM = 0.4;
	RIM_TINT = 0.5;
	ROUGHNESS = 0.7;
}

void light() {
	float n = floor(max(dot(NORMAL, LIGHT), 0.0) * bands) / bands;
	DIFFUSE_LIGHT += LIGHT_COLOR * n * ATTENUATION;
}
