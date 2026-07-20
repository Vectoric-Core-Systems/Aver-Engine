using Aver.Scripting;

// Exercises the Voxi C ABI from C#: reports what this GPU can do, then round-trips each setting.
// Run with:  dotnet run --project scripting/csharp/Aver.Scripting.Sample
// The native DLL is copied next to this exe by the csproj.

Console.WriteLine("Aver Engine — Voxi render settings (via C# scripting)");
Console.WriteLine(new string('-', 58));

Console.WriteLine($"GPU max MSAA      : {Voxi.MaxMsaa}x");
Console.WriteLine($"Supported MSAA    : {string.Join(", ", Voxi.SupportedMsaaCounts)}");
Console.WriteLine($"Ray tracing tier  : {Voxi.RayTracingTier}  (0=none, 10=DXR1.0, 11=DXR1.1)");
Console.WriteLine();

Console.WriteLine("Feature status:");
foreach (VoxiFeature f in Enum.GetValues<VoxiFeature>())
    Console.WriteLine($"  {Voxi.NameOf(f),-24} {Voxi.StatusOf(f),-16} {Voxi.StatusTextOf(f)}");
Console.WriteLine();

int failures = 0;
void Check(string what, bool ok)
{
    Console.WriteLine($"  [{(ok ? "PASS" : "FAIL")}] {what}");
    if (!ok) failures++;
}

Console.WriteLine("Round-trip checks:");

// MSAA is implemented, so setting it must stick (when the GPU supports the value).
foreach (int n in Voxi.SupportedMsaaCounts)
{
    Voxi.Msaa = n;
    Check($"MSAA = {n}x", Voxi.Msaa == n);
}

// An unsupported MSAA value must be rejected, not silently accepted.
int before = Voxi.Msaa;
Voxi.Msaa = 7;                       // never a legal sample count
Check("MSAA rejects illegal value 7", Voxi.Msaa == before);

// GI/RT/PT are not implemented yet, so they must refuse to turn on rather than lie.
foreach (var f in new[] { VoxiFeature.GlobalIllumination, VoxiFeature.RayTracing, VoxiFeature.PathTracing })
{
    bool accepted = Voxi.SetQuality(f, VoxiQuality.High);
    bool ready = Voxi.IsAvailable(f);
    Check($"{Voxi.NameOf(f)} honours its status ({Voxi.StatusOf(f)})",
          accepted == ready && (ready || Voxi.GetQuality(f) == VoxiQuality.Off));
}

// GI tunables are plain values and should always round-trip.
Voxi.VoxelResolution = 256; Check("Voxel grid = 256", Voxi.VoxelResolution == 256);
Voxi.GiIntensity = 1.75f;   Check("GI intensity = 1.75", Math.Abs(Voxi.GiIntensity - 1.75f) < 1e-5f);
Voxi.GiMaxDistance = 3000f; Check("GI distance = 3000", Math.Abs(Voxi.GiMaxDistance - 3000f) < 1e-3f);

Console.WriteLine();
Console.WriteLine(failures == 0 ? "All checks passed." : $"{failures} check(s) FAILED.");
return failures == 0 ? 0 : 1;
