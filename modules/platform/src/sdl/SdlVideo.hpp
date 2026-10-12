#pragma once

namespace aver::detail {

// SDL's video subsystem, brought up once per process with the engine's hints set first. Every SDL
// piece of Aver.Platform that needs a display (the window, the splash, the file dialog) calls this
// instead of SDL_Init so the hints cannot be skipped by whichever happens to run first. False, with
// the reason logged, when there is no display to talk to -- a headless CI runner, an SSH session.
bool sdlVideoUp();

} // namespace aver::detail
