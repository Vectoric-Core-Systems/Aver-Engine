// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
using Aver.UI;

namespace Sample.Game;

// The reference HUD: the default [AverHud] and what the editor's preview is tested against.

/// <summary>A HUD the engine can draw with no game running. Every number it draws is a field with a
/// default, so gameplay and the editor preview call the same Draw.</summary>
[AverHud("Sample HUD", Default = true)]
public sealed class SampleHud
{
    /// <summary>Health, 0..1.</summary>
    public float Health = 0.72f;

    /// <summary>Ammunition remaining, and the magazine size it is a fraction of.</summary>
    public int Ammo = 17;
    public int Magazine = 30;

    private float _pulse;

    /// <summary>Draws the health bar, the ammunition ticks and the reticle, sized in fractions of
    /// the viewport.</summary>
    public void Draw(float dt)
    {
        Rect vp = Hud.Viewport;
        float pad = vp.Height * 0.04f;
        float barW = vp.Width * 0.22f;
        float barH = vp.Height * 0.035f;

        _pulse += dt;
        float glow = 0.5f + 0.5f * (float)System.Math.Sin(_pulse * 2.0);

        // EVERY COORDINATE IS OFFSET BY THE VIEWPORT'S ORIGIN. Rect.X/Y were ignored and only
        // Width/Height used, so this HUD drew relative to the WINDOW. In the editor the viewport is
        // a dockspace node inset by the panels around it, so the health bar sat under the Content
        // Browser and the ammo ticks ran off the side -- and this file is the reference every
        // project copies its HUD from, so the mistake propagates.
        Hud.Bar(new Rect(vp.X + pad, vp.Y + vp.Height - pad - barH, barW, barH), Health,
                Colour.Rgb(20, 20, 24), Colour.Rgb(40, 16, 16),
                Colour.Lerp(Colour.Rgb(190, 40, 40), Colour.Rgb(70, 200, 90), Health));

        float tickW = vp.Width * 0.006f;
        float gap = tickW * 0.6f;
        for (int i = 0; i < Magazine; ++i)
        {
            float x = vp.X + vp.Width - pad - (Magazine - i) * (tickW + gap);
            Colour c = i < Ammo ? Colour.Rgb(220, 200, 120) : Colour.Rgb(48, 44, 38);
            Hud.Box(x, vp.Y + vp.Height - pad - barH, tickW, barH, c);
        }

        float r = vp.Height * 0.012f;
        float t = r * 0.22f;
        Colour tint = Colour.Rgb((byte)(180 + 60 * glow), (byte)(200 + 40 * glow), 220);
        // The crosshair marks the centre of the VIEWPORT, which is where the camera actually points.
        // Centred on the window instead, it sat off to one side of the image in the editor and told
        // the player their shot would go somewhere it would not.
        float cx = vp.X + vp.Width * 0.5f, cy = vp.Y + vp.Height * 0.5f;
        Hud.Box(cx - r, cy - t * 0.5f, r * 2.0f, t, tint);
        Hud.Box(cx - t * 0.5f, cy - r, t, r * 2.0f, tint);
    }
}
