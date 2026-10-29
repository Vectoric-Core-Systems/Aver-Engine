# Procedural sound (real-time sound graphs)

Four nodes that play a `.ocsnd` on the **audio thread** and steer it while it plays. The graph is authored in the Sound Editor; its
`Parameter` nodes are the named inputs (`rpm`, `intensity`, `surface`) these nodes set. See `docs/AUDIO.md` section 13.

- `sound=` is the `.ocsnd` path. The first play loads and compiles it (and the sounds its `WavePlayer` nodes name); later plays reuse it.
- **Looping** runs until a note-off (or `Stop Voice`). Not looping plays the graph's own duration, with any ADSR releasing before the end.
- **Seed** 0 picks a new seed every play, so `Noise` and `Random` vary; any other value repeats exactly.
- **Set Sound Graph Input** names the `Parameter` with `param=`. The value is latest-wins and smoothed on the audio thread, so a jump is a
  ramp rather than a click. Set the inputs the frame you play; to start already at a value (no ramp up from the default) use
  `Audio.PlayGraph(..., inputs)` from C#.
- **Note Off** lets an ADSR release from where it is; the voice ends after the longest release (10 ms of fade for a graph with no ADSR).
  **Stop Voice** (audio-control family) instead fades the voice out within one block.
- The voice pin works with the audio-control nodes: Set Voice Volume, Set Voice Position, Is Voice Playing, Stop Voice.
- Voice pitch does not apply to a graph voice; make the pitch a `Parameter` that feeds an oscillator or a `WavePlayer`.
