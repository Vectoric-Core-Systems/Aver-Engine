using Aver.UI;

namespace Sample.Game;

/// <summary>
/// A HUD the engine can draw with no game running: the reference [AverHud] and the thing the editor's
/// preview is tested against.
/// </summary>
/// <remarks>
/// <para>
/// The shape is the whole point. Every number it draws is a FIELD with a sensible default, and
/// <c>Draw</c> takes nothing but the frame's delta. Gameplay writes the fields; the editor writes
/// none of them and gets the defaults, which is why the same method serves both. A HUD whose
/// signature demanded its game's state -- <c>Draw(dt, shots, hits, recoil, cooldown)</c> -- could
/// only ever be called by that game.
/// </para>
/// <para>
/// It reads <see cref="Hud.Viewport"/> for its extents rather than assuming a size, so it lands
/// correctly whether it is drawn over a level viewport or inside an editor panel.
/// </para>
/// </remarks>
[AverHud("Sample HUD", Default = true)]
public sealed class SampleHud
{
    /// <summary>Health, 0..1. Written by gameplay; the editor never touches it.</summary>
    public float Health = 0.72f;

    /// <summary>Ammunition remaining, and the magazine size it is a fraction of.</summary>
    public int Ammo = 17;
    public int Magazine = 30;

    private float _pulse;

    public void Draw(float dt)
    {
        Rect vp = Hud.Viewport;
        // Everything is placed in FRACTIONS of the viewport, never in pixels. The preview panel is a
        // different size from the game's viewport and both have to look deliberate.
        float pad = vp.Height * 0.04f;
        float barW = vp.Width * 0.22f;
        float barH = vp.Height * 0.035f;

        // A slow pulse, so a still screenshot cannot be mistaken for a frozen one and so the dt
        // actually reaching Draw is visible.
        _pulse += dt;
        float glow = 0.5f + 0.5f * (float)System.Math.Sin(_pulse * 2.0);

        // Health, bottom left.
        Hud.Bar(new Rect(pad, vp.Height - pad - barH, barW, barH), Health,
                Colour.Rgb(20, 20, 24), Colour.Rgb(40, 16, 16),
                Colour.Lerp(Colour.Rgb(190, 40, 40), Colour.Rgb(70, 200, 90), Health));

        // Ammunition, bottom right, as a row of ticks -- readable at a glance, and it exercises the
        // draw list with more than one rectangle.
        float tickW = vp.Width * 0.006f;
        float gap = tickW * 0.6f;
        for (int i = 0; i < Magazine; ++i)
        {
            float x = vp.Width - pad - (Magazine - i) * (tickW + gap);
            Colour c = i < Ammo ? Colour.Rgb(220, 200, 120) : Colour.Rgb(48, 44, 38);
            Hud.Box(x, vp.Height - pad - barH, tickW, barH, c);
        }

        // A centre reticle, so there is something in the middle of the panel to look at.
        float r = vp.Height * 0.012f;
        float t = r * 0.22f;
        Colour tint = Colour.Rgb((byte)(180 + 60 * glow), (byte)(200 + 40 * glow), 220);
        Hud.Box(vp.Width * 0.5f - r, vp.Height * 0.5f - t * 0.5f, r * 2.0f, t, tint);
        Hud.Box(vp.Width * 0.5f - t * 0.5f, vp.Height * 0.5f - r, t, r * 2.0f, tint);
    }
}
