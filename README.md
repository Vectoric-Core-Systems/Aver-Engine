
# Aver Engine
-------------------------------------------------------------------------------------------------------------
A modular general purpose 3D engine developed by Hydrogen-isotope, design was derived from an earlier proprietary Sandbox Engine as well as Averion Engine, creator of Averion being https://github.com/xKrvZ . 

<img width="3839" height="2074" alt="NeonDistrictDemoSceneTAA" src="https://github.com/user-attachments/assets/4caa0d67-8089-4f7c-9c32-32ced2e1c8cf" />

#OVERVIEW

-------------------------------------------------------------------------------------------------------------
Aver Engine uses frontier NeuRaC(Neural Radiance Cache), NeuraFI (Neural Frame Interpolation), and ReSTIR PT to get state of the art rendering while maintaining an acceptable realtime framerate on majority of modern hardware (2020 and later), achieving quality of rendering similar to Unreal Engine or Unity HDRP while at comparably higher framerates. The engine also supports Aver Node Visual Scripting where you can use nodes and wires to link a graph together to form a script as well as C# Scripting. Anims and Assets are handled by the engine's importer which supports USD, GLTF, and GLB as of now. Materials are made in Visual Scripting as well as a Material graph where nodes define Physically Based Rendering(PBR) properties to construct realistic and accurate surfaces from Mettalic mirrors, to textured vents and rust on iron bars. 

#HOW TO USE
---------------------------------------------------------------------------------------------
You can choose to download from multiple areas, the release of the [Aver Launcher](https://github.com/Vectoric-Core-Systems/Aver-Launcher/releases) which automates part of Module Management for you as well as Aver Exchange directly integrating with the engine.
Alternatively you can download from the releases here but download the .zip folder from releases for compiled binary
You can also choose to download the entire source folder which has Engine Documentation, this README, GNU Lesser General Public License as well as the Source Code


Vendored third-party code keeps its own licence and copyright notices, and is not covered by the
LGPL:

| Path | Component | Licence |
|---|---|---|
| `third_party/imgui` | Dear ImGui | MIT |
| `third_party/stb` | stb single-file libraries | Public domain / MIT |
| `third_party/meshoptimizer` | meshoptimizer | MIT |
| `third_party/fonts` | Roboto, Material Icons | Apache-2.0 |
| `third_party/vulkan-headers` | Vulkan-Headers (Khronos) | Apache-2.0 |
| `third_party/fidelityfx-fsr` | AMD FidelityFX FSR 1 | MIT |
| `third_party/fidelityfx-denoiser` | AMD FidelityFX Denoiser | MIT |
| `modules/physics.jolt/Jolt` | Jolt Physics | MIT |
| `third_party/dxc-spirv` | DirectX Shader Compiler (`dxcompiler.dll`) | MIT + LLVM Release Licence (NCSA) |
| `third_party/nuget` | .NET Compiler Platform (Roslyn) | MIT |
