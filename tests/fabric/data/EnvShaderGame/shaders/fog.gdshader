shader_type fog;
uniform float density = 1.5;
uniform vec3 fog_albedo : source_color = vec3(0.8, 0.9, 1.0);
uniform vec4 glow = vec4(1.0, 0.5, 0.2, 2.0);
void fog() {
    float h = clamp(UVW.y, 0.0, 1.0);
    DENSITY = density * (1.0 - h) * (0.5 + 0.5 * sin(WORLD_POSITION.x * 0.5 + TIME));
    ALBEDO = fog_albedo;
    EMISSION = glow.rgb * glow.a * smoothstep(0.8, 1.0, UVW.x);
}
