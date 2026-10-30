// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 S1: AudioServer, the AudioEffect* resources, the AudioStream* resources and AudioStreamPlayer / AudioStreamPlayer3D / AudioListener3D
// against the engine's mixer (Aver.Audio.Abi) in OFFLINE mode: no device is opened, the test renders blocks into buffers itself. The DSP,
// the routing rules and Godot's 3D maths have their reference values in the C++ suites (AudioBusTest, AudioSpatialTest); what is checked here is the
// Godot surface on top: API names and results, the lifecycle of players (play, stop, finished, polyphony, pause), streams that pick or mix
// other streams, the layout, and what the converter wrote. The suite needs build/bin/Aver.Audio.Abi; without it every test is skipped.

using System.Reflection;
using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class AudioTests
{
    private const int Rate = 48000;
    private static bool s_native;

    public static int RunAll()
    {
        s_native = EnsureNative();
        if (!s_native)
        {
            Console.WriteLine("Test: audio (SKIP: build/bin/Aver.Audio.Abi was not found; build the Aver.Audio.Abi target)");
            return 0;
        }
        return T.Suite(() =>
        {
            Run("AudioServer: buses are added, named, found, moved, sent, muted and removed with Godot's rules", Buses);
            Run("AudioServer: volume, peaks, solo, mute, bypass and playback_speed_scale reach the mixer", BusMixing);
            Run("AudioServer: effects go on a bus, in order, and their properties change the sound", BusEffects);
            Run("AudioEffect classes: every managed default equals the engine's", EffectDefaults);
            Run("AudioEffectCapture, SpectrumAnalyzer and the compressor's sidechain through AudioServer", ReadableEffects);
            Run("AudioServer: layouts generate and apply", Layouts);
            Run("AudioStreamPlayer: play, stop, finished, volume, pitch, bus, pause, seek, position", PlayerBasics);
            Run("AudioStreamPlayer: max_polyphony cuts the oldest; autoplay; leaving the tree stops", PlayerPolyphony);
            Run("AudioStreamWAV: 8 and 16 bit data, stereo, length, loop points", WavStreams);
            Run("ResourceLoader: .wav / .ogg / .mp3 paths load as audio streams; a file plays", FilesLoad);
            Run("AudioStreamRandomizer: weights, no repeats, sequential, random pitch and volume", Randomizer);
            Run("AudioStreamPolyphonic: PlayStream, ids, volume, stop, the polyphony limit", Polyphonic);
            Run("AudioStreamGenerator: push frames, free space, skips, clear", Generator);
            Run("AudioStreamPlaylist, Synchronized and Interactive", Composites);
            Run("AudioStreamPlayer3D: the four attenuation models, unit_size, max_db, max_distance, panning", Player3DAttenuation);
            Run("AudioStreamPlayer3D: cone and attenuation filter, Doppler", Player3DConeDoppler);
            Run("AudioListener3D and the camera: who the 3D players hear from", Listener);
            Run("scene values: Object(...) streams and effects by their Godot property names", SceneValues);
            Run("animation audio tracks play through the players and trim the end", AnimationTracks);
            Run("the converter's AudioGame output: streams, players, bus layout", ConverterOutput);
        });
    }

    private static void Run(string name, Action body) =>
        T.Run(name, () => { AudioServer.LayoutOverride = null; AudioServer.ResetOffline(Rate, 2); AudioListener3D.Reset(); body(); });

    // ---- the native library ----

    private static string LibFile() =>
        OperatingSystem.IsWindows() ? "Aver.Audio.Abi.dll" : OperatingSystem.IsMacOS() ? "libAver.Audio.Abi.dylib" : "libAver.Audio.Abi.so";

    private static string? FindBin(string file)
    {
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string bin = Path.Combine(d.FullName, "build", "bin");
            if (File.Exists(Path.Combine(bin, file))) return bin;
        }
        return null;
    }

    private static bool EnsureNative()
    {
        string? bin = FindBin(LibFile());
        if (bin != null)
        {
            string beside = Path.Combine(AppContext.BaseDirectory, LibFile());
            try { if (!File.Exists(beside) || File.GetLastWriteTimeUtc(beside) < File.GetLastWriteTimeUtc(Path.Combine(bin, LibFile()))) File.Copy(Path.Combine(bin, LibFile()), beside, true); }
            catch (IOException) { /* a copy from an earlier run is in use */ }
            AudioNative.Reprobe();
        }
        return AudioNative.Available;
    }

    // ---- builders ----

    /// <summary>A WAV stream of 16-bit data holding a constant (so what comes out of the mixer IS the gain).</summary>
    private static AudioStreamWav Dc(float value, double seconds = 2.0, bool loop = false, int rate = Rate)
    {
        int frames = (int)(seconds * rate);
        byte[] data = new byte[frames * 2];
        short s = (short)Math.Round(value * 32768f);
        for (int i = 0; i < frames; i++) { data[2 * i] = (byte)(s & 0xFF); data[2 * i + 1] = (byte)((s >> 8) & 0xFF); }
        return new AudioStreamWav { Data = data, Format = AudioStreamWav.FormatEnum.Format16Bits, MixRate = rate, LoopMode = loop ? AudioStreamWav.LoopModeEnum.Forward : AudioStreamWav.LoopModeEnum.Disabled };
    }

    private static AudioStreamWav Tone(float hz, float amp, double seconds = 1.0)
    {
        int frames = (int)(seconds * Rate);
        byte[] data = new byte[frames * 2];
        for (int i = 0; i < frames; i++)
        {
            short s = (short)Math.Round(amp * MathF.Sin(MathF.Tau * hz * i / Rate) * 32767f);
            data[2 * i] = (byte)(s & 0xFF); data[2 * i + 1] = (byte)((s >> 8) & 0xFF);
        }
        return new AudioStreamWav { Data = data, Format = AudioStreamWav.FormatEnum.Format16Bits, MixRate = Rate };
    }

    private static AudioStreamPlayer Player(AudioStream s, string bus = "Master", float volumeDb = 0f)
    {
        var p = new AudioStreamPlayer { Stream = s, Bus = bus, VolumeDb = volumeDb };
        SceneTree.Instance.Root.AddChild(p);
        return p;
    }

    /// <summary>One frame of the game loop, then the same span of audio rendered by the offline mixer. Returns the stereo block.</summary>
    private static float[] Step(double dt = 0.01)
    {
        T.Frame(dt);
        return AudioServer.RenderOffline((int)Math.Round(dt * Rate));
    }

    /// <summary>The last left sample of a block.</summary>
    private static float Last(float[] block, int ch = 0) => block[block.Length - 2 + ch];

    private static float Rms(float[] block, int ch)
    {
        double s = 0; int n = block.Length / 2;
        for (int i = 0; i < n; i++) s += block[2 * i + ch] * block[2 * i + ch];
        return (float)Math.Sqrt(s / Math.Max(1, n));
    }

    private static TObj Make<TObj>(string cls) where TObj : class => (TObj)(object)ClassDB.Instantiate(cls).AsGodotObject()!;

    // ---- tests ----

    private static void Buses()
    {
        int changes = 0;
        void OnChange() => changes++;
        AudioServer.BusLayoutChanged += OnChange;
        try
        {
            T.Check("a new server is just Master", AudioServer.GetBusCount() == 1 && AudioServer.GetBusName(0) == "Master" && AudioServer.GetBusIndex("Master") == 0);
            AudioServer.AddBus();
            AudioServer.AddBus();
            T.Check("add_bus appends 'New Bus' and 'New Bus 2'", AudioServer.BusCount == 3 && AudioServer.GetBusName(1) == "New Bus" && AudioServer.GetBusName(2) == "New Bus 2");
            AudioServer.SetBusName(1, "Music");
            AudioServer.SetBusName(2, "SFX");
            T.Check("buses are found by name; an unknown name is -1", AudioServer.GetBusIndex("SFX") == 2 && AudioServer.GetBusIndex("Nope") == -1);
            T.Check("a bus sends to Master by default", AudioServer.GetBusSend(1) == "Master");
            AudioServer.SetBusSend(2, "Music");
            T.Check("and can send to a bus above it", AudioServer.GetBusSend(2) == "Music");
            AudioServer.AddBus(); AudioServer.SetBusName(3, "Ambience");
            AudioServer.MoveBus(1, 3);
            T.Check("move_bus(1, 3) puts Music after SFX (insert-before, numbered before the move)", AudioServer.GetBusName(1) == "SFX" && AudioServer.GetBusName(2) == "Music" && AudioServer.GetBusName(3) == "Ambience");
            AudioServer.MoveBus(0, 2);
            T.Check("Master does not move", AudioServer.GetBusName(0) == "Master");
            AudioServer.RemoveBus(0);
            T.Check("and cannot be removed", AudioServer.BusCount == 4);
            AudioServer.RemoveBus(3);
            T.Check("remove_bus closes the gap", AudioServer.BusCount == 3);
            AudioServer.SetBusVolumeDb(1, -12f);
            T.CheckNum("volume_db round trips", -12f, AudioServer.GetBusVolumeDb(1));
            T.CheckNum("volume_linear is its antilog", 0.25119f, AudioServer.GetBusVolumeLinear(1), 1e-4f);
            AudioServer.SetBusMute(1, true); AudioServer.SetBusSolo(2, true); AudioServer.SetBusBypassEffects(2, true);
            T.Check("mute, solo and bypass_fx are per bus", AudioServer.IsBusMute(1) && !AudioServer.IsBusMute(2) && AudioServer.IsBusSolo(2) && AudioServer.IsBusBypassingEffects(2));
            AudioServer.BusCount = 2;
            T.Check("bus_count can shrink", AudioServer.GetBusCount() == 2);
            T.Check("bus_layout_changed fired for the structural edits", changes >= 6, changes.ToString());
        }
        finally { AudioServer.BusLayoutChanged -= OnChange; }
    }

    private static void BusMixing()
    {
        AudioServer.AddBus(); AudioServer.SetBusName(1, "Music");
        AudioServer.AddBus(); AudioServer.SetBusName(2, "SFX");
        var music = Player(Dc(0.5f, 5, loop: true), "Music");
        var sfx = Player(Dc(0.25f, 5, loop: true), "SFX");
        music.Play(); sfx.Play();
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("two buses sum into Master", 0.75f, Last(Step()), 1e-3f);
        T.CheckNum("a bus's peak reads its level in dB (0.5 -> -6.02)", -6.0206f, AudioServer.GetBusPeakVolumeLeftDb(1, 0), 0.05f);
        T.CheckNum("Master's peak reads the sum (0.75 -> -2.5)", -2.499f, AudioServer.GetBusPeakVolumeRightDb(0, 0), 0.05f);
        AudioServer.SetBusVolumeDb(1, -6.0206f);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("bus volume halves it", 0.5f, Last(Step()), 1e-3f);
        AudioServer.SetBusMute(2, true);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("mute removes a bus", 0.25f, Last(Step()), 1e-3f);
        AudioServer.SetBusMute(2, false);
        AudioServer.SetBusSolo(2, true);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("solo leaves only the soloed bus (and Master)", 0.25f, Last(Step()), 1e-3f);
        AudioServer.SetBusSolo(2, false);
        AudioServer.SetBusVolumeDb(1, 0f);
        for (int i = 0; i < 3; i++) Step();
        AudioServer.PlaybackSpeedScale = 2f;
        T.CheckNum("playback_speed_scale is stored", 2f, AudioServer.PlaybackSpeedScale);
        double before = music.GetPlaybackPosition();
        for (int i = 0; i < 10; i++) Step(0.01);
        double adv = music.GetPlaybackPosition() - before;
        T.CheckNum("and plays everything twice as fast (0.1 s of audio, 0.2 s of sound)", 0.2f, (float)adv, 0.01f);
        T.Check("mix rate and the dummy driver", AudioServer.GetMixRate() == Rate && AudioServer.GetDriverName() == "Dummy");
    }

    private static void BusEffects()
    {
        AudioServer.AddBus(); AudioServer.SetBusName(1, "Music");
        var amp = new AudioEffectAmplify { VolumeDb = 6.0206f };
        AudioServer.AddBusEffect(1, amp);
        var pan = new AudioEffectPanner { Pan = -1f };
        AudioServer.AddBusEffect(1, pan, 0);
        T.Check("effects are counted and ordered (insert at 0)", AudioServer.GetBusEffectCount(1) == 2 && AudioServer.GetBusEffect(1, 0) == pan && AudioServer.GetBusEffect(1, 1) == amp);
        var p = Player(Dc(0.25f, 5, loop: true), "Music");
        p.Play();
        for (int i = 0; i < 3; i++) Step();
        float[] b = Step();
        T.CheckNum("Panner -1 then Amplify +6 dB: left 0.25 -> 0.5", 0.5f, Last(b, 0), 2e-3f);
        T.CheckNum("and the right channel is empty", 0f, Last(b, 1), 1e-4f);
        amp.VolumeDb = 0f;
        for (int i = 0; i < 2; i++) Step();
        T.CheckNum("a property change on the effect object reaches the mixer", 0.25f, Last(Step(), 0), 2e-3f);
        AudioServer.SetBusEffectEnabled(1, 0, false);
        for (int i = 0; i < 2; i++) Step();
        T.Check("a disabled effect is skipped", !AudioServer.IsBusEffectEnabled(1, 0) && T.Near(Last(Step(), 1), 0.25f, 2e-3f));
        AudioServer.SwapBusEffects(1, 0, 1);
        T.Check("swap_bus_effects reorders", AudioServer.GetBusEffect(1, 0) == amp && AudioServer.GetBusEffect(1, 1) == pan);
        AudioServer.RemoveBusEffect(1, 1);
        T.Check("remove_bus_effect takes one off", AudioServer.GetBusEffectCount(1) == 1);
        p.Stop();
        T.Check("a removed effect is detached from the engine and keeps its value", pan.NativeIds.Count == 0 && pan.Pan == -1f);
        var inst = AudioServer.GetBusEffectInstance(1, 0);
        T.Check("get_bus_effect_instance returns an instance", inst != null);
        // A low-pass on a 10 kHz tone.
        AudioServer.AddBus(); AudioServer.SetBusName(2, "Filtered");
        var lp = new AudioEffectLowPassFilter { CutoffHz = 500f, Resonance = 0.7071f, Db = AudioEffectFilter.FilterDB.Filter12Db };
        AudioServer.AddBusEffect(2, lp);
        var tone = Player(Tone(10000f, 0.5f, 2), "Filtered");
        tone.Play();
        for (int i = 0; i < 3; i++) Step();
        float filtered = Rms(Step(0.02), 0);
        AudioServer.SetBusEffectEnabled(2, 0, false);
        for (int i = 0; i < 3; i++) Step();
        float open = Rms(Step(0.02), 0);
        T.Check("a low-pass filter takes a 10 kHz tone down by more than 40 dB", filtered < open * 0.01f && open > 0.3f, $"{filtered} vs {open}");
    }

    private static void EffectDefaults()
    {
        AudioServer.AddBus();
        int checkedClasses = 0;
        var bad = new List<string>();
        foreach (Type t in typeof(AudioEffect).Assembly.GetTypes())
        {
            if (!typeof(AudioEffect).IsAssignableFrom(t) || t.IsAbstract || t.GetConstructor(Type.EmptyTypes) == null || t == typeof(AudioEffect)) continue;
            var detached = (AudioEffect)Activator.CreateInstance(t)!;
            if (detached.NativeClass.Length == 0) continue;
            var props = t.GetProperties(BindingFlags.Public | BindingFlags.Instance).Where(p => typeof(AudioEffect).IsAssignableFrom(p.DeclaringType!) && p.DeclaringType != typeof(AudioEffect)
                && p.CanRead && p.GetIndexParameters().Length == 0 && (p.PropertyType == typeof(float) || p.PropertyType == typeof(bool) || p.PropertyType == typeof(int) || p.PropertyType.IsEnum)).ToList();
            var managed = props.ToDictionary(p => p.Name, p => Convert.ToDouble(p.PropertyType == typeof(bool) ? ((bool)p.GetValue(detached)! ? 1 : 0) : Convert.ChangeType(p.GetValue(detached)!, p.PropertyType.IsEnum ? Enum.GetUnderlyingType(p.PropertyType) : p.PropertyType)));
            var attached = (AudioEffect)Activator.CreateInstance(t)!;
            AudioServer.AddBusEffect(1, attached);
            if (attached.NativeIds.Count == 0) { bad.Add(t.Name + " could not be added"); continue; }
            foreach (var p in props)
            {
                object v = p.GetValue(attached)!;
                double engine = Convert.ToDouble(p.PropertyType == typeof(bool) ? ((bool)v ? 1 : 0) : Convert.ChangeType(v, p.PropertyType.IsEnum ? Enum.GetUnderlyingType(p.PropertyType) : p.PropertyType));
                if (Math.Abs(engine - managed[p.Name]) > 1e-5) bad.Add($"{t.Name}.{p.Name}: managed {managed[p.Name]}, engine {engine}");
            }
            checkedClasses++;
            AudioServer.RemoveBusEffect(1, AudioServer.GetBusEffectCount(1) - 1);
        }
        T.Check("every effect class is added to a bus and has at least one property compared", checkedClasses >= 20, checkedClasses.ToString());
        T.Check("no managed default differs from the engine's", bad.Count == 0, string.Join("; ", bad));
        var eq = new AudioEffectEQ10();
        T.Check("EQ10 has ten bands named by frequency", eq.GetBandCount() == 10 && eq.GetBandFrequency(5) == 1000f);
        AudioServer.AddBusEffect(1, eq);
        eq.SetBandGainDb(5, 6f);
        T.CheckNum("band gains round trip through the engine", 6f, eq.GetBandGainDb(5));
        T.Check("the other bands stay at 0 dB", eq.GetBandGainDb(4) == 0f);
    }

    private static void ReadableEffects()
    {
        AudioServer.AddBus(); AudioServer.SetBusName(1, "Rec");
        var cap = new AudioEffectCapture { BufferLength = 0.1f };
        AudioServer.AddBusEffect(1, cap);
        var spec = new AudioEffectSpectrumAnalyzer();
        AudioServer.AddBusEffect(1, spec);
        float hz = Rate / 1024f * 21f;
        var p = Player(Tone(hz, 0.5f, 3), "Rec");
        p.Play();
        for (int i = 0; i < 6; i++) Step(0.02);
        T.Check("Capture has frames", cap.GetFramesAvailable() > 480 && cap.GetBufferLengthFrames() == 4800, cap.GetFramesAvailable().ToString());
        T.Check("can_get_buffer asks for frames", cap.CanGetBuffer(400) && !cap.CanGetBuffer(100000));
        Vector2[] got = cap.GetBuffer(480);
        T.Check("get_buffer returns that many stereo frames", got.Length == 480 && got.Any(v => MathF.Abs(v.X) > 0.1f));
        T.Check("and they are the tone (left = right, bounded by 0.5)", got.All(v => MathF.Abs(v.X - v.Y) < 1e-5f && MathF.Abs(v.X) <= 0.5001f));
        cap.ClearBuffer();
        T.Check("clear_buffer empties it", cap.GetFramesAvailable() == 0);
        var inst = (AudioEffectSpectrumAnalyzerInstance)AudioServer.GetBusEffectInstance(1, 1)!;
        Vector2 mag = inst.GetMagnitudeForFrequencyRange(hz - 20, hz + 20);
        T.Check("the analyser reads the tone's level where it is (0.5)", MathF.Abs(mag.X - 0.5f) < 0.03f && MathF.Abs(mag.Y - 0.5f) < 0.03f, mag.ToString());
        Vector2 off = inst.GetMagnitudeForFrequencyRange(5000, 8000, AudioEffectSpectrumAnalyzerInstance.MagnitudeMode.Average);
        T.Check("and nothing where there is none", off.X < 0.01f);

        // Sidechain: Music is ducked by the Voice bus.
        p.Stop();
        for (int i = 0; i < 5; i++) Step(0.02);
        AudioServer.AddBus(); AudioServer.SetBusName(2, "Music");
        AudioServer.AddBus(); AudioServer.SetBusName(3, "Voice");
        var comp = new AudioEffectCompressor { Threshold = -20f, Ratio = 4f, Sidechain = "Voice" };
        AudioServer.AddBusEffect(2, comp);
        var music = Player(Dc(0.05f, 5, loop: true), "Music");
        music.Play();
        for (int i = 0; i < 40; i++) Step(0.02);
        float quiet = Last(Step());
        var voice = Player(Dc(0.8f, 5, loop: true), "Voice");
        voice.Play();
        for (int i = 0; i < 40; i++) Step(0.02);
        float ducked = Last(Step()) - 0.8f;
        T.Check("without the Voice bus playing Music is not compressed (under the threshold)", T.Near(quiet, 0.05f, 2e-3f));
        T.Check("with it, the sidechain pushes Music down", ducked < 0.05f * 0.6f && ducked > 0f, ducked.ToString());
    }

    private static void Layouts()
    {
        AudioServer.AddBus(); AudioServer.SetBusName(1, "Music");
        AudioServer.SetBusVolumeDb(1, -4f);
        AudioServer.AddBusEffect(1, new AudioEffectReverb { RoomSize = 0.3f, Wet = 0.2f });
        AudioBusLayout layout = AudioServer.GenerateBusLayout();
        T.Check("generate_bus_layout writes the buses and effects", layout.Text.Contains("BUS \"Music\"") && layout.Text.Contains("AudioEffectReverb") && layout.Text.Contains("room_size=0.3"));
        AudioServer.RemoveBus(1);
        T.Check("a bus is gone", AudioServer.BusCount == 1);
        AudioServer.SetBusLayout(layout);
        T.Check("set_bus_layout brings it back", AudioServer.BusCount == 2 && AudioServer.GetBusName(1) == "Music" && T.Near(AudioServer.GetBusVolumeDb(1), -4f));
        var rv = AudioServer.GetBusEffect(1, 0) as AudioEffectReverb;
        T.Check("and its effect comes back as the right class with the right values", rv != null && T.Near(rv.RoomSize, 0.3f) && T.Near(rv.Wet, 0.2f));
        rv!.Wet = 0.9f;
        T.CheckNum("an effect from a layout is live too", 0.9f, rv.Wet);

        // The project's layout file is applied by the first use of the server.
        string dir = Path.Combine(Path.GetTempPath(), "aver-audio-layout-" + System.Environment.ProcessId);
        Directory.CreateDirectory(dir);
        try
        {
            string file = Path.Combine(dir, "default_bus_layout.ocbus");
            File.WriteAllText(file, "OCBUS 1\nBUS \"Master\" volume_db=-1\nBUS \"Game\" send=\"Master\" volume_db=-9\nEFFECT \"AudioEffectLimiter\" enabled=1 ceiling_db=-3\n");
            AudioServer.LayoutOverride = file;
            AudioServer.ResetOffline(Rate, 2);
            T.Check("AUDIO.BUSLAYOUT: the project's layout is applied when audio starts", AudioServer.GetBusIndex("Game") == 1 && T.Near(AudioServer.GetBusVolumeDb(1), -9f) && T.Near(AudioServer.GetBusVolumeDb(0), -1f));
            T.Check("with its effect parameters", AudioServer.GetBusEffect(1, 0) is AudioEffectLimiter lim && T.Near(lim.CeilingDb, -3f));
        }
        finally
        {
            AudioServer.LayoutOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }

    private static void PlayerBasics()
    {
        AudioServer.AddBus(); AudioServer.SetBusName(1, "Music");
        var p = Player(Dc(0.5f, 0.5), "Master");
        int finished = 0;
        p.Finished += () => finished++;
        T.Check("a player starts stopped", !p.Playing && !p.HasStreamPlayback());
        p.Play();
        T.Check("play() makes it playing at once", p.Playing && p.HasStreamPlayback() && p.GetStreamPlayback() != null);
        Step(); Step();
        T.CheckNum("and the mixer plays the stream", 0.5f, Last(Step()), 1e-3f);
        T.CheckNum("the playback position is the time played (0.03 s)", 0.03f, (float)p.GetPlaybackPosition(), 0.005f);
        for (int i = 0; i < 50; i++) Step();
        T.Check("when the stream ends the player stops and finished fires once", !p.Playing && finished == 1, $"playing={p.Playing} finished={finished}");

        p.VolumeDb = -6.0206f;
        p.Play();
        Step(); Step();
        T.CheckNum("volume_db -6.02 halves it", 0.25f, Last(Step()), 1e-3f);
        p.VolumeDb = 0f;
        Step(); Step();
        T.CheckNum("volume_db is live", 0.5f, Last(Step()), 1e-3f);
        p.Stop();
        finished = 0;
        for (int i = 0; i < 3; i++) Step();
        T.Check("stop() stops without finished", !p.Playing && finished == 0);

        p.PitchScale = 2f;
        p.Play();
        for (int i = 0; i < 10; i++) Step(0.01);
        T.CheckNum("pitch_scale 2 plays twice as fast", 0.2f, (float)p.GetPlaybackPosition(), 0.01f);
        p.Stop();
        p.PitchScale = 1f;

        p.Bus = "Music";
        p.Play();
        Step(); Step(); Step();
        AudioServer.SetBusMute(1, true);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("the bus is looked up by name each frame (a muted Music bus silences it)", 0f, Last(Step()), 1e-4f);
        p.Bus = "NoSuchBus";
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("an unknown bus plays on Master", 0.5f, Last(Step()), 1e-3f);
        p.Stop();

        p.Bus = "Master";
        p.Stream = Dc(0.5f, 5, loop: false);
        p.Play();
        for (int i = 0; i < 5; i++) Step(0.01);
        double at = p.GetPlaybackPosition();
        p.StreamPaused = true;
        Step(0.01); Step(0.01); Step(0.01);
        double held = p.GetPlaybackPosition();
        float silent = Last(Step(0.01));
        T.Check("stream_paused holds the playhead and is silent", T.Near((float)held, (float)p.GetPlaybackPosition(), 1e-6f) && silent == 0f && held - at < 0.025, $"{at} {held} {silent}");
        T.Check("and it is still playing", p.Playing);
        p.StreamPaused = false;
        for (int i = 0; i < 4; i++) Step(0.01);
        T.Check("unpausing resumes", p.GetPlaybackPosition() > held + 0.02);

        p.Seek(2.0);
        Step(0.01); Step(0.01);
        T.CheckNum("seek() moves the playhead", 2.02f, (float)p.GetPlaybackPosition(), 0.02f);

        // Tree pause pauses players and unpausing resumes them.
        SceneTree.Instance.Paused = true;
        Step(0.01);
        double frozen = p.GetPlaybackPosition();
        Step(0.01); Step(0.01);
        T.Check("pausing the tree pauses its players", T.Near((float)frozen, (float)p.GetPlaybackPosition(), 1e-6f));
        SceneTree.Instance.Paused = false;
        for (int i = 0; i < 5; i++) Step(0.01);
        T.Check("and unpausing lets them go on", p.GetPlaybackPosition() > frozen + 0.02);
    }

    private static void PlayerPolyphony()
    {
        var p = Player(Dc(0.2f, 5, loop: true));
        p.MaxPolyphony = 2;
        p.Play(); p.Play(); p.Play();
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("three plays with max_polyphony 2 sound as two (0.2 + 0.2)", 0.4f, Last(Step()), 2e-3f);
        p.MaxPolyphony = 1;
        p.Play();
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("with the default 1 a new play cuts the old", 0.2f, Last(Step()), 2e-3f);
        p.Stop();

        var auto = new AudioStreamPlayer { Stream = Dc(0.3f, 5, loop: true), Autoplay = true };
        SceneTree.Instance.Root.AddChild(auto);
        T.Check("autoplay starts when the node is ready", auto.Playing);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("and is heard", 0.3f, Last(Step()), 2e-3f);
        auto.GetParent()!.RemoveChild(auto);
        for (int i = 0; i < 3; i++) Step();
        T.Check("a player that leaves the tree stops", !auto.Playing && Last(Step()) == 0f);
        auto.QueueFree();
    }

    private static void WavStreams()
    {
        var s16 = Dc(0.5f, 0.25);
        T.CheckNum("get_length is the data's duration", 0.25f, (float)s16.GetLength(), 1e-4f);
        var s8 = new AudioStreamWav { Data = Enumerable.Repeat((byte)64, Rate / 2).ToArray(), Format = AudioStreamWav.FormatEnum.Format8Bits, MixRate = Rate };
        var p8 = Player(s8);
        p8.Play();
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("8-bit data is signed (64 -> 0.5)", 0.5f, Last(Step()), 1e-3f);
        p8.Stop();
        T.Check("an 8-bit mono stream is monophonic; a stereo one is not", s8.IsMonophonic() && !new AudioStreamWav { Stereo = true }.IsMonophonic());

        byte[] st = new byte[Rate / 2 * 4];
        for (int i = 0; i < st.Length / 4; i++)
        {
            st[4 * i] = 0x00; st[4 * i + 1] = 0x20;        // left 0.25
            st[4 * i + 2] = 0x00; st[4 * i + 3] = 0xE0;    // right -0.25
        }
        var sp = Player(new AudioStreamWav { Data = st, Format = AudioStreamWav.FormatEnum.Format16Bits, MixRate = Rate, Stereo = true });
        sp.Play();
        for (int i = 0; i < 3; i++) Step();
        float[] b = Step();
        T.Check("stereo data keeps its channels", T.Near(Last(b, 0), 0.25f, 1e-3f) && T.Near(Last(b, 1), -0.25f, 1e-3f));
        sp.Stop();

        var looped = Dc(0.4f, 0.05);
        looped.LoopMode = AudioStreamWav.LoopModeEnum.Forward;
        var lp = Player(looped);
        lp.Play();
        for (int i = 0; i < 12; i++) Step();
        T.Check("a looping stream keeps sounding past its end", lp.Playing && T.Near(Last(Step()), 0.4f, 2e-3f));
        lp.Stop();
        var adpcm = new AudioStreamWav { Data = new byte[100], Format = AudioStreamWav.FormatEnum.FormatImaAdpcm };
        var ap = Player(adpcm);
        ap.Play();
        T.Check("IMA ADPCM data is refused with a warning, not played", !ap.Playing && T.Logs.Any(l => l.Message.Contains("ImaAdpcm")));
    }

    private static string WriteWav(string dir, string name, float value, double seconds, int rate = Rate)
    {
        int frames = (int)(seconds * rate);
        string path = Path.Combine(dir, name);
        using var f = File.Create(path);
        using var w = new BinaryWriter(f);
        w.Write("RIFF".ToCharArray()); w.Write(36 + frames * 2); w.Write("WAVE".ToCharArray());
        w.Write("fmt ".ToCharArray()); w.Write(16); w.Write((short)1); w.Write((short)1); w.Write(rate); w.Write(rate * 2); w.Write((short)2); w.Write((short)16);
        w.Write("data".ToCharArray()); w.Write(frames * 2);
        short s = (short)Math.Round(value * 32768f);
        for (int i = 0; i < frames; i++) w.Write(s);
        return path;
    }

    private static void FilesLoad()
    {
        string dir = Path.Combine(Path.GetTempPath(), "aver-audio-files-" + System.Environment.ProcessId);
        Directory.CreateDirectory(dir);
        try
        {
            WriteWav(dir, "beep.wav", 0.5f, 1.0);
            File.WriteAllBytes(Path.Combine(dir, "song.ogg"), new byte[16]);
            File.WriteAllBytes(Path.Combine(dir, "song.mp3"), new byte[16]);
            ResourceLoader.ContentRootOverride = dir;
            T.Check("a .wav loads as AudioStreamWav", ResourceLoader.Load<AudioStream>("res://beep.wav") is AudioStreamWav);
            T.Check("a .ogg loads as AudioStreamOggVorbis", ResourceLoader.Load<AudioStream>("res://song.ogg") is AudioStreamOggVorbis);
            T.Check("a .mp3 loads as AudioStreamMP3", ResourceLoader.Load<AudioStream>("res://song.mp3") is AudioStreamMP3);
            var wav = ResourceLoader.Load<AudioStream>("res://beep.wav")!;
            T.CheckNum("its length comes from the file", 1f, (float)wav.GetLength(), 1e-3f);
            var p = Player(wav);
            p.Play();
            for (int i = 0; i < 3; i++) Step();
            T.CheckNum("and the file plays", 0.5f, Last(Step()), 2e-3f);
            p.Stop();
            var bad = new AudioStreamOggVorbis { ResourcePath = "res://song.ogg" };
            var pb = Player(bad);
            pb.Play();
            T.Check("a file the platform cannot decode does not play and says so", !pb.Playing && T.Logs.Any(l => l.Message.Contains("could not decode")));
            T.Check("a stream with no file does not play", !Player(new AudioStreamOggVorbis { ResourcePath = "res://missing.ogg" }).Playing);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }

    private static void Randomizer()
    {
        var a = Dc(0.1f, 5, true); var b = Dc(0.2f, 5, true); var c = Dc(0.4f, 5, true);
        var r = new AudioStreamRandomizer();
        r.AddStream(0, a); r.AddStream(1, b, 3f); r.AddStream(2, c);
        T.Check("streams are counted, ordered and weighted", r.StreamsCount == 3 && r.GetStream(1) == b && r.GetStreamProbabilityWeight(1) == 3f);
        r.MoveStream(2, 0);
        T.Check("move_stream", r.GetStream(0) == c && r.GetStream(2) == b);
        r.MoveStream(0, 2); r.RemoveStream(2); r.AddStream(2, b, 3f);
        T.Check("remove_stream and add_stream", r.StreamsCount == 3 && r.GetStream(2) == b);

        var seq = new Queue<float>();
        AudioStreamRandomizer.Rng = () => seq.Count > 0 ? seq.Dequeue() : 0f;
        try
        {
            // weights 1, 3, 1 (total 5): r*5 in [0,1) -> a, [1,4) -> b, [4,5) -> c
            r.PlaybackMode = AudioStreamRandomizer.PlaybackModeEnum.Random;
            seq.Enqueue(0.1f); T.Check("random: 0.1 picks the first", r.Pick() == 0);
            seq.Enqueue(0.5f); T.Check("random: 0.5 falls in the heavy second", r.Pick() == 1);
            seq.Enqueue(0.95f); T.Check("random: 0.95 picks the last", r.Pick() == 2);
            r.PlaybackMode = AudioStreamRandomizer.PlaybackModeEnum.RandomNoRepeats;
            seq.Enqueue(0.5f); int first = r.Pick();
            // the same random number must not give the same stream twice running
            seq.Enqueue(0.5f); int second = r.Pick();
            T.Check("random no repeats: the same draw does not repeat the last stream", first != second, $"{first} {second}");
            bool anyRepeat = false;
            int last = r.Pick();
            var rng = new Random(7);
            AudioStreamRandomizer.Rng = () => (float)rng.NextDouble();
            for (int i = 0; i < 300; i++) { int n = r.Pick(); anyRepeat |= n == last; last = n; }
            T.Check("random no repeats: 300 draws never repeat", !anyRepeat);
            r.PlaybackMode = AudioStreamRandomizer.PlaybackModeEnum.Sequential;
            T.CheckSeq("sequential: in order and wrapping", new[] { 0, 1, 2, 0, 1 }, Enumerable.Range(0, 5).Select(_ => r.Pick()));

            // pitch and volume
            AudioStreamRandomizer.Rng = () => seq.Count > 0 ? seq.Dequeue() : 0f;
            var one = new AudioStreamRandomizer { RandomPitch = 2f, PlaybackMode = AudioStreamRandomizer.PlaybackModeEnum.Random };
            one.AddStream(0, Dc(0.3f, 5, true));
            var p = Player(one);
            seq.Enqueue(0f); seq.Enqueue(1f);        // pick, then the pitch draw: 1 -> the top of [0.5, 2]
            p.Play();
            for (int i = 0; i < 10; i++) Step(0.01);
            T.CheckNum("random_pitch 2 with the top draw plays twice as fast", 0.2f, (float)p.GetPlaybackPosition(), 0.01f);
            p.Stop();
            var vol = new AudioStreamRandomizer { RandomVolumeOffsetDb = 6.0206f, PlaybackMode = AudioStreamRandomizer.PlaybackModeEnum.Random };
            vol.AddStream(0, Dc(0.2f, 5, true));
            var pv = Player(vol);
            seq.Enqueue(0f); seq.Enqueue(1f);        // pick, then the volume draw: +6 dB
            pv.Play();
            for (int i = 0; i < 3; i++) Step();
            T.CheckNum("random_volume_offset_db: the top draw is +6 dB (0.2 -> 0.4)", 0.4f, Last(Step()), 3e-3f);
            pv.Stop();
            T.Check("an empty randomizer plays nothing", !Player(new AudioStreamRandomizer()).Playing);
        }
        finally { AudioStreamRandomizer.Rng = () => GD.Randf(); }
    }

    private static void Polyphonic()
    {
        var poly = new AudioStreamPolyphonic { Polyphony = 2 };
        var p = Player(poly);
        p.Play();
        var pb = p.GetStreamPlayback() as AudioStreamPlaybackPolyphonic;
        T.Check("play() gives a polyphonic playback that is playing before any stream", pb != null && p.Playing);
        long a = pb!.PlayStream(Dc(0.2f, 5, true));
        long b = pb.PlayStream(Dc(0.1f, 5, true), 0f, 0f, 1f);
        T.Check("play_stream returns ids", a >= 1 && b > a && pb.IsStreamPlaying(a) && pb.IsStreamPlaying(b));
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("both are mixed", 0.3f, Last(Step()), 2e-3f);
        pb.SetStreamVolumeDb(a, -6.0206f);
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("set_stream_volume_db changes one of them", 0.2f, Last(Step()), 2e-3f);
        long c = pb.PlayStream(Dc(0.05f, 5, true));
        T.Check("a third stream cuts the oldest (polyphony 2)", c > b && !pb.IsStreamPlaying(a) && pb.IsStreamPlaying(b) && pb.IsStreamPlaying(c));
        pb.StopStream(b);
        T.Check("stop_stream stops one", !pb.IsStreamPlaying(b) && pb.IsStreamPlaying(c));
        T.Check("a null stream gives the invalid id", pb.PlayStream(null) == AudioStreamPlaybackPolyphonic.InvalidId);
        long d = pb.PlayStream(Dc(0.1f, 0.05));
        for (int i = 0; i < 12; i++) Step();
        T.Check("a stream that ends frees its slot", !pb.IsStreamPlaying(d) && p.Playing);
        pb.SetStreamPitchScale(c, 2f);
    }

    private static void Generator()
    {
        var gen = new AudioStreamGenerator { MixRate = Rate, BufferLength = 0.1f };
        var p = Player(gen);
        p.Play();
        var pb = p.GetStreamPlayback() as AudioStreamGeneratorPlayback;
        T.Check("a generator's playback is the push interface", pb != null && p.Playing);
        T.Check("the buffer starts empty (frames available = its size)", pb!.GetFramesAvailable() == 4800 && pb.CanPushBuffer(4800) && !pb.CanPushBuffer(4801));
        var frames = Enumerable.Range(0, 2400).Select(_ => new Vector2(0.3f, -0.3f)).ToArray();
        T.Check("push_buffer takes frames", pb.PushBuffer(frames) && pb.GetFramesAvailable() == 2400);
        T.Check("and refuses a push that does not fit, taking nothing", !pb.PushBuffer(Enumerable.Range(0, 2401).Select(_ => Vector2.Zero).ToArray()) && pb.GetFramesAvailable() == 2400);
        T.Check("push_frame pushes one", pb.PushFrame(new Vector2(0.3f, -0.3f)) && pb.GetFramesAvailable() == 2399);
        float[] b = Step();
        T.Check("the pushed frames are heard in order", T.Near(Last(b, 0), 0.3f, 2e-3f) && T.Near(Last(b, 1), -0.3f, 2e-3f));
        T.Check("pushed frames are consumed as they play", pb.GetFramesAvailable() > 2399);
        for (int i = 0; i < 8; i++) Step();
        T.Check("running dry counts skips and keeps the player going", pb.GetSkips() >= 1 && p.Playing, pb.GetSkips().ToString());
        pb.PushBuffer(frames);
        pb.ClearBuffer();
        Step();
        T.Check("clear_buffer drops what is queued", pb.GetFramesAvailable() == 4800);
    }

    private static void Composites()
    {
        var list = new AudioStreamPlaylist { StreamCount = 2, Loop = false };
        list.SetListStream(0, Dc(0.2f, 0.05));
        list.SetListStream(1, Dc(0.4f, 0.05));
        var p = Player(list);
        int finished = 0;
        p.Finished += () => finished++;
        p.Play();
        Step(0.01);
        float first = Last(Step(0.01));
        for (int i = 0; i < 4; i++) Step(0.01);
        float second = Last(Step(0.01));
        T.Check("a playlist plays its streams one after the other (0.2, then 0.4)", T.Near(first, 0.2f, 3e-3f) && T.Near(second, 0.4f, 3e-3f), $"{first} {second}");
        for (int i = 0; i < 20; i++) Step(0.01);
        T.Check("and without loop it ends and the player finishes once", !p.Playing && finished == 1);
        var loopList = new AudioStreamPlaylist { StreamCount = 1, Loop = true };
        loopList.SetListStream(0, Dc(0.2f, 0.03));
        var lp = Player(loopList);
        lp.Play();
        for (int i = 0; i < 30; i++) Step(0.01);
        T.Check("with loop it goes round again", lp.Playing);
        lp.Stop();

        var sync = new AudioStreamSynchronized { StreamCount = 2 };
        sync.SetSyncStream(0, Dc(0.2f, 5, true));
        sync.SetSyncStream(1, Dc(0.1f, 5, true));
        sync.SetSyncStreamVolume(1, -6.0206f);
        var sp = Player(sync);
        sp.Play();
        for (int i = 0; i < 3; i++) Step();
        T.CheckNum("a synchronized stream sums its streams, each at its volume (0.2 + 0.05)", 0.25f, Last(Step()), 2e-3f);
        T.CheckNum("its length is the longest", 5f, (float)sync.GetLength(), 1e-3f);
        sp.Stop();

        var inter = new AudioStreamInteractive { ClipCount = 2 };
        inter.SetClipName(0, "calm"); inter.SetClipStream(0, Dc(0.1f, 5, true));
        inter.SetClipName(1, "battle"); inter.SetClipStream(1, Dc(0.4f, 5, true));
        var ip = Player(inter);
        ip.Play();
        var ipb = (AudioStreamPlaybackInteractive)ip.GetStreamPlayback()!;
        for (int i = 0; i < 3; i++) Step();
        T.Check("an interactive stream starts at its initial clip", ipb.GetCurrentClipIndex() == 0 && T.Near(Last(Step()), 0.1f, 3e-3f));
        ipb.SwitchToClipByName("battle");
        for (int i = 0; i < 3; i++) Step();
        T.Check("switch_to_clip_by_name changes the clip", ipb.GetCurrentClipIndex() == 1 && T.Near(Last(Step()), 0.4f, 3e-3f));
        inter.AddTransition(0, 1, AudioStreamInteractive.TransitionFromTime.NextBar, AudioStreamInteractive.TransitionToTime.Start, AudioStreamInteractive.FadeMode.Cross, 1f);
        T.Check("transition rules are kept", inter.HasTransition(0, 1) && !inter.HasTransition(1, 0));
    }

    // ---- 3D ----

    private static AudioStreamPlayer3D Player3D(AudioStream s, Vector3 at, string bus = "Master")
    {
        var p = new AudioStreamPlayer3D { Stream = s, Bus = bus, UnitSize = 1f };
        SceneTree.Instance.Root.AddChild(p);
        p.GlobalPosition = at;
        return p;
    }

    private static float Settled3D(AudioStreamPlayer3D p, int ch = 0) { for (int i = 0; i < 3; i++) Step(); return Last(Step(), ch); }

    private static void Player3DAttenuation()
    {
        var cam = new Camera3D { Current = true };
        SceneTree.Instance.Root.AddChild(cam);
        var ahead = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        ahead.Play();
        float centre = Settled3D(ahead);
        T.CheckNum("dead ahead at 5 m, unit_size 1: 0.5 * 0.2 * sqrt(0.5)", 0.5f * 0.2f * 0.70711f, centre, 1e-3f);
        T.CheckNum("and both ears get the same", Last(Step(), 0), Last(Step(), 1), 1e-5f);
        ahead.Stop();

        var right = Player3D(Dc(0.5f, 5, true), new Vector3(5, 0, 0));
        right.Play();
        Settled3D(right);
        float[] b = Step();
        T.Check("a source to the right favours the right ear (0.447 / 0.894 of the distance gain)", T.Near(Last(b, 0) / (0.5f * 0.2f), 0.4472f, 5e-3f) && T.Near(Last(b, 1) / (0.5f * 0.2f), 0.8944f, 5e-3f));
        right.PanningStrength = 0f;
        T.CheckNum("panning_strength 0 centres it", Last(Step(), 0), Last(Step(), 1), 2e-3f);
        right.Stop();

        var sq = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        sq.AttenuationModel = AudioStreamPlayer3D.AttenuationModelEnum.InverseSquareDistance;
        sq.Play();
        T.CheckNum("inverse square distance: 1/25 at 5 m", 0.5f * 0.04f * 0.70711f, Settled3D(sq), 5e-4f);
        sq.AttenuationModel = AudioStreamPlayer3D.AttenuationModelEnum.Logarithmic;
        Step(); Step();
        T.CheckNum("logarithmic: -20 ln(5) dB", 0.5f * MathF.Pow(10f, -20f * MathF.Log(5f) / 20f) * 0.70711f, Last(Step()), 5e-4f);
        sq.AttenuationModel = AudioStreamPlayer3D.AttenuationModelEnum.Disabled;
        Step(); Step();
        T.CheckNum("disabled: no distance attenuation at all (0 dB, under the +3 dB clamp)", 0.5f * 0.70711f, Last(Step()), 1e-3f);
        sq.Stop();

        var unit = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        unit.UnitSize = 10f;
        unit.Play();
        T.CheckNum("unit_size 10: 5 m is inside one unit, so the clamp (+3 dB) applies", 0.5f * 1.4125f * 0.70711f, Settled3D(unit), 1e-3f);
        unit.Stop();

        var lim = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        lim.MaxDistance = 10f;
        lim.AttenuationModel = AudioStreamPlayer3D.AttenuationModelEnum.Disabled;
        lim.MaxDb = 0f;
        lim.Play();
        T.CheckNum("max_distance 10 at 5 m: half gain", 0.5f * 0.5f * 0.70711f, Settled3D(lim), 1e-3f);
        lim.GlobalPosition = new Vector3(0, 0, -12);
        T.CheckNum("and silent beyond it", 0f, Settled3D(lim), 1e-4f);
        lim.Stop();

        var vol = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        vol.VolumeDb = -20f;
        vol.Play();
        T.CheckNum("volume_db adds to the attenuation (-20 dB at 5 m)", 0.5f * 0.02f * 0.70711f, Settled3D(vol), 3e-4f);
        vol.Stop();

        var moving = Player3D(Dc(0.5f, 5, true), new Vector3(0, 0, -5));
        moving.Play();
        Settled3D(moving);
        moving.GlobalPosition = new Vector3(0, 0, -10);
        T.CheckNum("moving the node moves the sound (10 m: 0.1)", 0.5f * 0.1f * 0.70711f, Settled3D(moving), 5e-4f);
    }

    private static void Player3DConeDoppler()
    {
        var cam = new Camera3D { Current = true };
        SceneTree.Instance.Root.AddChild(cam);
        var hi = Tone(10000f, 0.5f, 3);
        var lo = Tone(200f, 0.5f, 3);
        float RmsAt(AudioStream s, Action<AudioStreamPlayer3D> setup)
        {
            var p = Player3D(s, new Vector3(0, 0, -5));
            p.PanningStrength = 0f;
            setup(p);
            p.Play();
            for (int i = 0; i < 3; i++) Step();
            float r = Rms(Step(0.02), 0);
            p.Stop();
            for (int i = 0; i < 4; i++) Step();
            return r;
        }
        float loFar = RmsAt(lo, _ => { });
        float hiFar = RmsAt(hi, _ => { });
        T.Check("the attenuation filter: at 5 m a 10 kHz tone is far below a 200 Hz one", hiFar < loFar * 0.1f && loFar > 0.01f, $"{hiFar} {loFar}");
        float hiOff = RmsAt(hi, p => p.AttenuationFilterCutoffHz = 20500f);
        float loOff = RmsAt(lo, p => p.AttenuationFilterCutoffHz = 20500f);
        T.Check("with the cutoff at 20500 Hz the filter is off and only distance matters", T.Near(hiOff, loOff, loOff * 0.1f), $"{hiOff} {loOff}");

        // The cone: the emitter faces -Z (towards the camera at the origin when it sits at -5? it is at z = -5, so -Z points AWAY from the camera).
        float inCone = RmsAt(hi, p => { p.EmissionAngleEnabled = true; p.EmissionAngleDegrees = 45f; p.AttenuationFilterCutoffHz = 5000f; p.RotationDegrees = new Vector3(0, 180, 0); });
        float outCone = RmsAt(hi, p => { p.EmissionAngleEnabled = true; p.EmissionAngleDegrees = 45f; });
        T.Check("the emission cone: the same source facing the listener is louder in the highs than facing away", inCone > outCone * 3f, $"{inCone} {outCone}");

        // Doppler
        var mover = Player3D(Dc(0.1f, 20, true), new Vector3(0, 0, -100));
        mover.DopplerTracking = AudioStreamPlayer3D.DopplerTrackingEnum.IdleStep;
        mover.Play();
        Step(1.0 / 60);
        double start = mover.GetPlaybackPosition();
        float speed = 34.3f;
        double dt = 1.0 / 60;
        for (int i = 0; i < 60; i++)
        {
            mover.GlobalPosition += new Vector3(0, 0, speed * (float)dt);
            Step(dt);
        }
        double played = mover.GetPlaybackPosition() - start;
        T.Check("a source approaching at 34.3 m/s plays 1.111x fast (Doppler)", Math.Abs(played - 1.0 * 1.1111) < 0.03, played.ToString());
        mover.DopplerTracking = AudioStreamPlayer3D.DopplerTrackingEnum.Disabled;
        Step(dt);
        double s2 = mover.GetPlaybackPosition();
        for (int i = 0; i < 30; i++) Step(dt);
        T.Check("and at the normal rate with tracking off", Math.Abs(mover.GetPlaybackPosition() - s2 - 0.5) < 0.02);
    }

    private static void Listener()
    {
        var cam = new Camera3D { Current = true };
        SceneTree.Instance.Root.AddChild(cam);
        var src = Player3D(Dc(0.5f, 5, true), new Vector3(5, 0, 0));
        src.Play();
        Settled3D(src);
        float[] b = Step();
        T.Check("with only a camera, a source on its right is heard on the right", Last(b, 1) > Last(b, 0) * 1.5f);
        var l = new AudioListener3D();
        SceneTree.Instance.Root.AddChild(l);
        T.Check("the first AudioListener3D in the tree is current", l.IsCurrent() && l.GetListenerTransform().Origin == Vector3.Zero);
        l.GlobalPosition = new Vector3(10, 0, 0);
        Settled3D(src);
        b = Step();
        T.Check("a listener to the right of the source reverses the pan: the source is now on its left", Last(b, 0) > Last(b, 1) * 1.5f);
        var l2 = new AudioListener3D();
        SceneTree.Instance.Root.AddChild(l2);
        T.Check("a second listener does not take over", l.IsCurrent() && !l2.IsCurrent());
        l2.MakeCurrent();
        T.Check("make_current does", l2.IsCurrent() && !l.IsCurrent());
        l2.ClearCurrent();
        T.Check("clear_current lets go", !l2.IsCurrent());
        Settled3D(src);
        b = Step();
        T.Check("with no current listener the camera is used again", Last(b, 1) > Last(b, 0) * 1.5f);
    }

    private static void SceneValues()
    {
        var wav = Dc(0.1f, 1);
        var rnd = Make<AudioStreamRandomizer>("AudioStreamRandomizer");
        rnd.Set("streams_count", 2);
        rnd.Set("stream_0/stream", wav);
        rnd.Set("stream_0/weight", 2.0);
        rnd.Set("stream_1/stream", wav);
        rnd.Set("random_pitch", 1.5);
        rnd.Set("playback_mode", 1);
        T.Check("a randomizer is built from its scene properties (streams_count, stream_N/stream, weight)", rnd.StreamsCount == 2 && rnd.GetStream(0) == wav && rnd.GetStreamProbabilityWeight(0) == 2f
                && rnd.RandomPitch == 1.5f && rnd.PlaybackMode == AudioStreamRandomizer.PlaybackModeEnum.Random);
        var sync = Make<AudioStreamSynchronized>("AudioStreamSynchronized");
        sync.Set("stream_count", 2); sync.Set("stream_1", wav); sync.Set("stream_1/volume", -3.0);
        T.Check("a synchronized stream: stream_count, stream_N, stream_N/volume", sync.StreamCount == 2 && sync.GetSyncStream(1) == wav && T.Near(sync.GetSyncStreamVolume(1), -3f));
        var inter = Make<AudioStreamInteractive>("AudioStreamInteractive");
        inter.Set("clip_count", 2); inter.Set("clip_1/name", "b"); inter.Set("clip_1/stream", wav); inter.Set("clip_1/auto_advance", 1); inter.Set("clip_1/next_clip", 0);
        T.Check("an interactive stream: clip_count and clip_N/...", inter.ClipCount == 2 && inter.GetClipName(1) == "b" && inter.GetClipStream(1) == wav && inter.GetClipAutoAdvance(1) == AudioStreamInteractive.AutoAdvanceMode.Enabled);
        var chorus = Make<AudioEffectChorus>("AudioEffectChorus");
        chorus.Set("voice/2/delay_ms", 33.0);
        chorus.Set("wet", 0.25);
        T.Check("an effect takes indexed Godot names (voice/2/delay_ms) and plain ones", T.Near(chorus.GetVoiceDelayMs(1), 33f) && T.Near(chorus.Wet, 0.25f));
        var eq = Make<AudioEffectEQ6>("AudioEffectEQ6");
        eq.Set("band_db/1000_hz", 4.0);
        T.CheckNum("and band_db/<hz>_hz", 4f, eq.GetBandGainDb(3));
        var pl = Make<AudioStreamPlayer3D>("AudioStreamPlayer3D");
        pl.Set("attenuation_model", 2); pl.Set("unit_size", 4.0); pl.Set("max_db", 6.0); pl.Set("emission_angle_enabled", true); pl.Set("doppler_tracking", 1); pl.Set("volume_db", -3.0);
        T.Check("AudioStreamPlayer3D properties by Godot name", pl.AttenuationModel == AudioStreamPlayer3D.AttenuationModelEnum.Logarithmic && pl.UnitSize == 4f && pl.MaxDb == 6f && pl.EmissionAngleEnabled
                && pl.DopplerTracking == AudioStreamPlayer3D.DopplerTrackingEnum.IdleStep && T.Near(pl.VolumeDb, -3f));
        var d = new AudioStreamPlayer3D();
        T.Check("Godot's AudioStreamPlayer3D defaults", d.UnitSize == 10f && d.MaxDb == 3f && d.MaxDistance == 0f && d.PanningStrength == 1f && d.EmissionAngleDegrees == 45f && d.EmissionAngleFilterAttenuationDb == -12f
                && d.AttenuationFilterCutoffHz == 5000f && d.AttenuationFilterDb == -24f && d.MaxPolyphony == 1 && d.AreaMask == 1u && d.Bus == "Master");
        var f = new AudioStreamPlayer();
        T.Check("and AudioStreamPlayer's", f.VolumeDb == 0f && f.PitchScale == 1f && f.Bus == "Master" && f.MaxPolyphony == 1 && !f.Autoplay && !f.StreamPaused && f.MixTarget == AudioStreamPlayer.MixTargetEnum.Stereo);
    }

    private static void AnimationTracks()
    {
        string dir = Path.Combine(Path.GetTempPath(), "aver-audio-anim-" + System.Environment.ProcessId);
        Directory.CreateDirectory(dir);
        try
        {
            WriteWav(dir, "hit.wav", 0.5f, 0.4);
            ResourceLoader.ContentRootOverride = dir;
            var p = Player(new AudioStreamWav());
            ((IAnimationAudioTarget)p).AnimationPlayStream("res://hit.wav", 0.1, 0.0);
            T.Check("an animation's audio track sets the stream and plays it", p.Playing && p.Stream is AudioStreamWav);
            for (int i = 0; i < 4; i++) Step(0.01);
            T.CheckNum("from the start offset", 0.14f, (float)p.GetPlaybackPosition(), 0.02f);
            ((IAnimationAudioTarget)p).AnimationStopStream();
            T.Check("and stops when the animation does", !p.Playing);

            // end_offset trims the end: 0.4 s long, 0.2 s trimmed -> stops near 0.2 s
            ((IAnimationAudioTarget)p).AnimationPlayStream("res://hit.wav", 0.0, 0.2);
            for (int i = 0; i < 12; i++) Step(0.01);
            T.Check("with end_offset 0.2 on a 0.4 s file it is still playing at 0.12 s", p.Playing);
            for (int i = 0; i < 12; i++) Step(0.01);
            T.Check("and stops once it has reached length - end_offset", !p.Playing);

            // The whole chain: an AnimationPlayer's audio track drives a player in the tree.
            var root = new Node3D { Name = "Root" };
            var anim = new Animation { Length = 1f };
            int au = anim.AddTrack(Animation.TrackType.Audio);
            anim.TrackSetPath(au, "Sound");
            anim.AudioTrackInsertKey(au, 0.1, new AudioStreamWav { ResourcePath = "res://hit.wav" }, 0f, 0f);
            var ap = new AnimationPlayer { Name = "Anim" };
            var lib = new AnimationLibrary(); lib.AddAnimation("go", anim);
            ap.AddAnimationLibrary("", lib);
            var snd = new AudioStreamPlayer { Name = "Sound" };
            root.AddChild(ap); root.AddChild(snd);
            SceneTree.Instance.Root.AddChild(root);
            ap.Play("go");
            T.Frame(0.0);
            Step(0.05);
            T.Check("an AnimationPlayer with an audio track: silent before the key", !snd.Playing);
            Step(0.1);
            T.Check("and playing once the key has passed", snd.Playing && snd.Stream is AudioStreamWav);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            try { Directory.Delete(dir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }

    private static void ConverterOutput()
    {
        string? bin = FindBin("AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : ""));
        if (bin == null) { Console.WriteLine("  SKIP: build/bin/AverAssetC was not found"); return; }
        string fixture = Path.Combine(T.DataDir(), "AudioGame");
        string outDir = Path.Combine(Path.GetTempPath(), "aver-fabric-audio-conv-" + System.Environment.ProcessId);
        string data = outDir + "-src";
        try
        {
            // The fixture is text only; the sound files are made beside a copy of it.
            foreach (string f in Directory.EnumerateFiles(fixture, "*", SearchOption.AllDirectories))
            {
                string to = Path.Combine(data, Path.GetRelativePath(fixture, f));
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                File.Copy(f, to, true);
            }
            WriteWav(Path.Combine(data, "sfx"), "loop.wav", 0.3f, 0.5);
            WriteWav(Path.Combine(data, "sfx"), "step_a.wav", 0.2f, 0.1);
            WriteWav(Path.Combine(data, "sfx"), "step_b.wav", 0.4f, 0.1);
            File.WriteAllBytes(Path.Combine(data, "sfx", "song.ogg"), new byte[16]);
            File.WriteAllBytes(Path.Combine(data, "sfx", "jingle.mp3"), new byte[16]);
            var psi = new System.Diagnostics.ProcessStartInfo(Path.Combine(bin, "AverAssetC" + (OperatingSystem.IsWindows() ? ".exe" : "")), $"fabric \"{data}\" \"{outDir}\" --scenes-only")
            { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            using var proc = System.Diagnostics.Process.Start(psi)!;
            proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
            proc.WaitForExit();
            T.Check("the converter runs on the AudioGame fixture", proc.ExitCode == 0, "exit " + proc.ExitCode);
            if (proc.ExitCode != 0) return;

            string content = Path.Combine(outDir, "AudioGame", "Content");
            ResourceLoader.ContentRootOverride = content;
            AudioServer.LayoutOverride = "Audio/default_bus_layout.ocbus";
            AudioServer.ResetOffline(Rate, 2);
            T.Check("the bus layout is applied: Music and SFX exist, Music sends to Master", AudioServer.GetBusIndex("Music") == 1 && AudioServer.GetBusIndex("SFX") == 2 && AudioServer.GetBusSend(1) == "Master");
            T.Check("volumes, mute and solo come across", T.Near(AudioServer.GetBusVolumeDb(1), -6f) && AudioServer.IsBusMute(2) && !AudioServer.IsBusSolo(1));
            T.Check("the Music bus has its reverb and the SFX bus its compressor with a sidechain",
                    AudioServer.GetBusEffect(1, 0) is AudioEffectReverb rv && T.Near(rv.RoomSize, 0.6f) && AudioServer.GetBusEffect(2, 0) is AudioEffectCompressor cp && cp.Sidechain == "Music" && T.Near(cp.Threshold, -18f));

            var scene = ResourceLoader.Load<PackedScene>("res://main.tscn");
            T.Check("the converted level loads", scene != null);
            if (scene == null) return;
            Node main = scene.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            var music = main.GetNode<AudioStreamPlayer>("Music");
            T.Check("the 2D player: stream, bus, volume and autoplay", music.Stream is AudioStreamWav && music.Bus == "Music" && T.Near(music.VolumeDb, -3f) && music.Autoplay && music.Playing);
            var three = main.GetNode<AudioStreamPlayer3D>("Emitter");
            T.Check("the 3D player keeps its Godot properties", three.Stream is AudioStreamWav && three.UnitSize == 2f && three.MaxDb == 6f && three.AttenuationModel == AudioStreamPlayer3D.AttenuationModelEnum.InverseSquareDistance
                    && three.EmissionAngleEnabled && T.Near(three.EmissionAngleDegrees, 30f) && three.MaxDistance == 40f && three.DopplerTracking == AudioStreamPlayer3D.DopplerTrackingEnum.IdleStep);
            var rnd = main.GetNode<AudioStreamPlayer>("Steps").Stream as AudioStreamRandomizer;
            T.Check("a randomizer resource keeps its streams, weights and pitch range", rnd != null && rnd.StreamsCount == 2 && rnd.GetStreamProbabilityWeight(1) == 3f && rnd.RandomPitch == 1.2f);
            T.Check("a looping WAV import keeps its loop mode", main.GetNode<AudioStreamPlayer>("Music").Stream is AudioStreamWav w && w.LoopMode == AudioStreamWav.LoopModeEnum.Forward);
            for (int i = 0; i < 5; i++) Step();
            T.Check("and the whole thing makes sound", Rms(Step(0.02), 0) > 1e-4f);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = null;
            AudioServer.LayoutOverride = null;
            try { Directory.Delete(outDir, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
            try { Directory.Delete(data, true); } catch (IOException) { /* a leftover temp folder is harmless */ }
        }
    }
}
