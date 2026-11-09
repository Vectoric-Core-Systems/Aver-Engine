// Generates the golden vectors in PcgGolden.hpp from the F# reference (Aver.Pcg).
//   dotnet fsi pcg_golden.fsx > golden.txt   then   python pcg_golden_to_hpp.py golden.txt PcgGolden.hpp
#load "../../../scripting/fsharp/Aver.Pcg/Types.fs"
#load "../../../scripting/fsharp/Aver.Pcg/Pcg.fs"
#load "../../../scripting/fsharp/Aver.Pcg/Primitives.fs"
open Aver.Pcg
let inv = System.Globalization.CultureInfo.InvariantCulture
let f (x: float32) = x.ToString("R", inv)
printfn "// hash"
for x in [0u;1u;2u;12345u;0xFFFFFFFFu;0xDEADBEEFu] do printfn "H %u %u" x (Rand.hash x)
printfn "// hash2"
for (s,i) in [(0,0);(1,2);(-7,100);(123456,-3);(42,99999)] do printfn "H2 %d %d %u" s i (Rand.hash2 s i)
printfn "// hash3"
for (s,x,y,z) in [(0,0,0,0);(5,1,2,3);(-9,-1,-2,-3);(777,100,-200,300)] do printfn "H3 %d %d %d %d %u" s x y z (Rand.hash3 s x y z)
printfn "// float01"
for h in [0u;255u;256u;0x80000000u;0xFFFFFFFFu;12345678u] do printfn "F %u %s" h (f (Rand.float01 h))
printfn "// range"
printfn "R 9 4 -10 10 %s" (f (Rand.range 9 4 (-10.0f) 10.0f))
let mk (x:float32) (y:float32) (z:float32) : Vec3 = { X = x; Y = y; Z = z }
let region : Bounds = { Min = mk (-500.0f) (-300.0f) 0.0f; Max = mk 1500.0f 700.0f 100.0f }
let spec = { ScatterSpec.Default with Region = region; Count=12; Seed=31337; Assets=[|"a";"b";"c"|]; RandomYaw=true; ScaleMin=0.5f; ScaleMax=2.0f; MinSpacing=0.0f }
printfn "// scatter free"
for p in Pcg.scatter spec do printfn "S %s %s %s %s %s %s %s %s %s %s" (f p.Position.X) (f p.Position.Y) (f p.Position.Z) (f p.Rotation.Z) (f p.Rotation.W) (f p.Scale.X) p.Asset "0" "0" "0"
printfn "// scatter spaced"
for p in Pcg.scatter { spec with Count=60; MinSpacing=250.0f; Seed=99 } do printfn "T %s %s %s %s %s %s %s" (f p.Position.X) (f p.Position.Y) (f p.Position.Z) (f p.Rotation.Z) (f p.Rotation.W) (f p.Scale.X) p.Asset
printfn "// infinite"
let isp = Pcg.infiniteExpanse 1234 1600.0f 2
for (x,y,z) in [(0.0f,0.0f,0.0f);(1700.0f,-300.0f,5.0f);(-5000.0f,4000.0f,0.0f);(123.5f,456.5f,789.5f)] do printfn "I %s %s %s %s" (f x) (f y) (f z) (f (Pcg.sampleInfinite isp x y z))
