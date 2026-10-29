shader_type sky;
uniform vec3 top_color : source_color = vec3(0.1, 0.3, 0.8);
uniform vec3 horizon_color : source_color = vec3(0.7, 0.8, 0.9);
uniform float sun_size = 0.02;
uniform int star_density = 3;
uniform bool show_stars = true;
uniform sampler2D noise_tex : hint_default_black;
global uniform float wind_strength;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void sky() {
    float h = clamp(EYEDIR.y, 0.0, 1.0);
    vec3 col = mix(horizon_color, top_color, pow(h, 0.5));
    float sun = smoothstep(1.0 - sun_size, 1.0, dot(EYEDIR, LIGHT0_DIRECTION));
    col += LIGHT0_COLOR * LIGHT0_ENERGY * sun;
    if (show_stars && !AT_CUBEMAP_PASS) {
        vec2 g = floor(EYEDIR.xz / max(EYEDIR.y, 0.05) * float(star_density) * 20.0);
        col += vec3(step(0.995, hash(g))) * (1.0 - h);
    }
    col += texture(noise_tex, SKY_COORDS).rgb * 0.01 + vec3(sin(TIME * 0.1) * 0.01) + vec3(wind_strength * 0.0);
    COLOR = col;
}
