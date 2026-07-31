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

        Hud.Bar(new Rect(pad, vp.Height - pad - barH, barW, barH), Health,
                Colour.Rgb(20, 20, 24), Colour.Rgb(40, 16, 16),
                Colour.Lerp(Colour.Rgb(190, 40, 40), Colour.Rgb(70, 200, 90), Health));

        float tickW = vp.Width * 0.006f;
        float gap = tickW * 0.6f;
        for (int i = 0; i < Magazine; ++i)
        {
            float x = vp.Width - pad - (Magazine - i) * (tickW + gap);
            Colour c = i < Ammo ? Colour.Rgb(220, 200, 120) : Colour.Rgb(48, 44, 38);
            Hud.Box(x, vp.Height - pad - barH, tickW, barH, c);
        }

        float r = vp.Height * 0.012f;
        float t = r * 0.22f;
        Colour tint = Colour.Rgb((byte)(180 + 60 * glow), (byte)(200 + 40 * glow), 220);
        Hud.Box(vp.Width * 0.5f - r, vp.Height * 0.5f - t * 0.5f, r * 2.0f, t, tint);
        Hud.Box(vp.Width * 0.5f - t * 0.5f, vp.Height * 0.5f - r, t, r * 2.0f, tint);
    }
}
