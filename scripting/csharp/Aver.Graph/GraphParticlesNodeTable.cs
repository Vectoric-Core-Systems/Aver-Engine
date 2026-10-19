// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The Particles v2 rows of the game-systems node table (docs/plans/wave10/GPU_PARTICLES.md section 2.13). Every method
// is in ParticlesGraph (Aver.Framework/ParticlesGraph.cs); effect paths and user-parameter names are NODE attributes,
// never pins. The four older emitter nodes (AN_SetEmitterEnabled, AN_SetEmitterEffect, AN_RestartEmitter,
// AN_SpawnEmitterAt) stay in the RenderProps family and keep their keys; this family adds no duplicate.
// Palette mirror: sandbox/src/GraphNodeDefsParticles.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] ParticlesTable() => new[]
    {
        Reg("an_spawnparticlesat", GenericKind.Exec, typeof(ParticlesGraph), "SpawnParticlesAtForGraph", "x:float,y:float,z:float,dirX:float=0,dirY:float=0,dirZ:float=1,count:int=1", "spawned:int", "p:x,p:y,p:z,p:dirX,p:dirY,p:dirZ,p:count,@effect", "spawned"),
        Reg("an_setparticlefloat", GenericKind.Exec, typeof(ParticlesGraph), "SetParticleFloatForGraph", "entity:int,value:float", "success:bool", "p:entity,@param,p:value", "success"),
        Reg("an_setparticlevector", GenericKind.Exec, typeof(ParticlesGraph), "SetParticleVectorForGraph", "entity:int,x:float,y:float,z:float", "success:bool", "p:entity,@param,p:x,p:y,p:z", "success"),
        Reg("an_setparticlecolor", GenericKind.Exec, typeof(ParticlesGraph), "SetParticleColorForGraph", "entity:int,r:float,g:float,b:float,a:float=1", "success:bool", "p:entity,@param,p:r,p:g,p:b,p:a", "success"),
        Reg("an_burstemitter", GenericKind.Exec, typeof(ParticlesGraph), "BurstEmitterForGraph", "entity:int,count:int=10", "success:bool", "p:entity,p:count", "success"),
        Reg("an_stopemitter", GenericKind.Exec, typeof(ParticlesGraph), "StopEmitterForGraph", "entity:int,killParticles:bool=false", "success:bool", "p:entity,p:killParticles", "success"),
        Reg("an_setemittermodifiers", GenericKind.Exec, typeof(ParticlesGraph), "SetEmitterModifiersForGraph", "entity:int,rateScale:float=1,sizeScale:float=1,speedScale:float=1", "success:bool", "p:entity,p:rateScale,p:sizeScale,p:speedScale", "success"),
        Reg("an_getemitterstate", GenericKind.Pure, typeof(ParticlesGraph), "GetEmitterStateForGraph", "entity:int", "alive:int,onGpu:bool,found:bool", "p:entity,>alive,>onGpu", "found"),
        Reg("an_getparticlestats", GenericKind.Pure, typeof(ParticlesGraph), "GetParticleStatsForGraph", "", "gpuEmitters:int,cpuEmitters:int,gpuParticles:int,cpuParticles:int,ok:bool", ">gpuEmitters,>cpuEmitters,>gpuParticles,>cpuParticles", "ok"),
    };
}
