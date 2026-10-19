// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// A speed, gear and revs readout for a vehicle: binds a game-UI layout by widget names to one vehicle's telemetry.

using System.Globalization;
using Aver.UI;

namespace Aver.Framework;

/// <summary>Binds a layout to a vehicle. Widgets are found by name and any may be absent: <c>Speed</c> (text), <c>Gear</c> (text), <c>Rpm</c>
/// (text), <c>RpmBar</c> (progress, 0..1 of <see cref="RpmMax"/>), <c>Health</c> (progress, 0..1), <c>AbsLamp</c> and <c>TcsLamp</c> (text or
/// anything with Visible, shown while the aid is working). Call <see cref="Update"/> once a frame.</summary>
public sealed class VehicleHud
{
    private readonly Widget _speed, _gear, _rpm, _rpmBar, _health, _absLamp, _tcsLamp;
    private readonly Entity _vehicle;
    private readonly bool _miles;

    /// <summary>The rev limit the bar is drawn against, rpm.</summary>
    public float RpmMax { get; set; } = 6000f;

    /// <summary>Finds the widgets under <paramref name="root"/> and remembers the vehicle.</summary>
    public VehicleHud(Widget root, Entity vehicle, bool miles = false)
    {
        _vehicle = vehicle;
        _miles = miles;
        _speed = root.Find("Speed");
        _gear = root.Find("Gear");
        _rpm = root.Find("Rpm");
        _rpmBar = root.Find("RpmBar");
        _health = root.Find("Health");
        _absLamp = root.Find("AbsLamp");
        _tcsLamp = root.Find("TcsLamp");
    }

    /// <summary>Reads the vehicle's telemetry and writes the widgets. Does nothing when the vehicle is not built.</summary>
    public void Update()
    {
        if (!_vehicle.TryGetVehicleTelemetry(out VehicleTelemetry t)) return;
        if (_speed.Id != 0) _speed.Text = FormatSpeed(t.ForwardSpeedCmS, _miles);
        if (_gear.Id != 0) _gear.Text = FormatGear(t.Gear);
        if (_rpm.Id != 0) _rpm.Text = ((int)MathF.Round(t.Rpm)).ToString(CultureInfo.InvariantCulture);
        if (_rpmBar.Id != 0) _rpmBar.Value = Math.Clamp(t.Rpm / MathF.Max(1f, RpmMax), 0f, 1f);
        if (_health.Id != 0) _health.Value = Math.Clamp(t.Health01, 0f, 1f);
        if (_absLamp.Id != 0) _absLamp.Visible = t.AbsActive;
        if (_tcsLamp.Id != 0) _tcsLamp.Visible = t.TcsActive;
    }

    /// <summary>A speed in cm/s as text: "100 km/h" (or "62 mph" with <paramref name="miles"/>). Always positive: a reversing car reads its speed.</summary>
    public static string FormatSpeed(float cmPerSecond, bool miles = false)
    {
        float v = MathF.Abs(cmPerSecond);
        float shown = miles ? v * 0.0223694f : v * 0.036f;
        return ((int)MathF.Round(shown)).ToString(CultureInfo.InvariantCulture) + (miles ? " mph" : " km/h");
    }

    /// <summary>The gear as the dash shows it: R, N, then the number.</summary>
    public static string FormatGear(int gear) => gear < 0 ? "R" : gear == 0 ? "N" : gear.ToString(CultureInfo.InvariantCulture);
}
