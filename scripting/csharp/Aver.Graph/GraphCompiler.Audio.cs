// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Audio nodes: sounds, listener and bus volume.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// The five exec-capable audio nodes. Playing a sound, stopping one, moving the ears and moving
    /// a slider are all side effects; IsSoundPlaying is the one pure read and is not in this list.
    private static bool IsExecCapableAudioType(string type) => type.ToLowerInvariant() switch
    {
        "playsound" or "playsoundat" or "stopsound" or "setlistener" or "setbusvolume" => true,
        _ => false,
    };

    /// Runs one audio node's call at the point the exec walk reaches it. Same shape as EmitExecSpawn:
    /// push the sound= literal where there is one (no string pin it could arrive on), pull the
    /// ordinary pins, Call, capture or discard.
    private void EmitExecAudio(Node node)
    {
        if (_il == null) return;
        string t = node.Type.ToLowerInvariant();

        if (t == "playsound" || t == "playsoundat")
        {
            if (string.IsNullOrEmpty(node.SoundPath))
                throw new InvalidOperationException(
                    $"{node.Type} node '{node.Id}' has no sound= attribute naming the file to play");
            _il.Emit(OpCodes.Ldstr, node.SoundPath);
            if (t == "playsoundat")
            {
                EmitPullInput(node, "x");
                EmitPullInput(node, "y");
                EmitPullInput(node, "z");
            }
            EmitPullInput(node, "volume");
            EmitPullInput(node, "pitch");
            EmitPullInput(node, "looping");
            EmitPullInput(node, "bus");
            if (t == "playsoundat")
            {
                EmitPullInput(node, "innerCm");
                EmitPullInput(node, "outerCm");
            }
            var voiceLocal = _il.DeclareLocal(typeof(int));
            _il.Emit(OpCodes.Ldloca, voiceLocal);
            _il.Emit(OpCodes.Call, t == "playsound" ? PlaySoundMethod : PlaySoundAtMethod);

            if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
                _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
            else
                _il.Emit(OpCodes.Pop);

            if (node.Pins.Any(p => p.IsOutput && p.Name == "voice"))
            {
                _il.Emit(OpCodes.Ldloc, voiceLocal);
                _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "voice", typeof(int)));
            }
            return;
        }

        switch (t)
        {
            case "stopsound":     EmitPullInput(node, "voice");  _il.Emit(OpCodes.Call, StopSoundMethod); break;
            case "setlistener":   EmitPullInput(node, "entity"); _il.Emit(OpCodes.Call, SetListenerMethod); break;
            case "setbusvolume":
                EmitPullInput(node, "bus");
                EmitPullInput(node, "volume");
                _il.Emit(OpCodes.Call, SetBusVolumeMethod);
                break;
            default: return;
        }
        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else
            _il.Emit(OpCodes.Pop);
    }
    private static readonly MethodInfo PlaySoundMethod =
        typeof(GraphInterop).GetMethod("PlaySoundForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlaySoundForGraph was not found by reflection");
    private static readonly MethodInfo PlaySoundAtMethod =
        typeof(GraphInterop).GetMethod("PlaySoundAtForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PlaySoundAtForGraph was not found by reflection");
    private static readonly MethodInfo StopSoundMethod =
        typeof(GraphInterop).GetMethod("StopSoundForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.StopSoundForGraph was not found by reflection");
    private static readonly MethodInfo IsSoundPlayingMethod =
        typeof(GraphInterop).GetMethod("IsSoundPlayingForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.IsSoundPlayingForGraph was not found by reflection");
    private static readonly MethodInfo SetListenerMethod =
        typeof(GraphInterop).GetMethod("SetListenerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetListenerForGraph was not found by reflection");
    private static readonly MethodInfo SetBusVolumeMethod =
        typeof(GraphInterop).GetMethod("SetBusVolumeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBusVolumeForGraph was not found by reflection");
}
