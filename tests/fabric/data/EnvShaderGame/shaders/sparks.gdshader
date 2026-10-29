shader_type particles;
uniform vec4 gravity_and_drag = vec4(0.0, -9.8, 0.0, 0.5);
uniform vec3 emit_dir = vec3(0.0, 1.0, 0.0);
uniform float spin = 2.0;
float rand(uint seed) {
    seed = seed * 747796405u + 2891336453u;
    uint w = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967295.0;
}
void start() {
    float r = rand(RANDOM_SEED);
    VELOCITY = (EMISSION_TRANSFORM * vec4(emit_dir * (1.0 + r), 0.0)).xyz;
    COLOR = vec4(1.0, 0.5 + 0.5 * r, 0.2, 1.0);
    CUSTOM = vec4(r, 0.0, 0.0, 0.0);
}
void process() {
    VELOCITY += gravity_and_drag.xyz * DELTA;
    VELOCITY *= max(0.0, 1.0 - gravity_and_drag.w * DELTA);
    TRANSFORM[3].xyz += VELOCITY * DELTA;
    if (TRANSFORM[3].y < -5.0) { ACTIVE = false; }
}
