// Shared by the fixture shaders.
#define WAVE_SCALE 2.0
float wave_at(float x, float t) {
	return sin(x * WAVE_SCALE + t);
}
