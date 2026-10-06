// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The rows of the game-systems node table: type, kind, the class and method it calls, its pins, and
// how the method's parameters are fed (see GenericNodeSpec.Args). Palette rows for the editor are the
// hand-kept mirror in sandbox/src/GraphNodeDefsGameSystems.hpp (and GraphNodeDefsSynapseAi.hpp);
// GameSystemNodes.Validate() checks every row against the real method it names.

using Aver.Framework;
using Aver.Prefab;
using Aver.Scene;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] Table() => new[]
    {
        Reg("an_onevent", GenericKind.Trigger, typeof(GraphTimerEvents), "", "", "", "", null),
        Reg("an_settimer", GenericKind.Exec, typeof(GraphTimerEvents), "SetTimerForGraph", "target:int,delay:float=1,looping:bool", "handle:int", "p:target,p:delay,p:looping,@event", "handle"),
        Reg("an_cleartimer", GenericKind.Exec, typeof(GraphTimerEvents), "ClearTimerForGraph", "handle:int", "cleared:bool", "p:handle", "cleared"),
        Reg("an_dispatchevent", GenericKind.Exec, typeof(GraphTimerEvents), "DispatchEventForGraph", "sender:int,target:int,i:int,f:float,immediate:bool", "", "p:sender,p:target,p:i,p:f,p:immediate,@event", null),
        Reg("an_eventpayload", GenericKind.Exec, typeof(GraphTimerEvents), "ReadEventPayloadForGraph", "index:int", "sender:int,target:int,i:int,f:float,b:bool", "p:index,>sender,>target,>i,>f,>b", null),
        Reg("getblackboardfloat", GenericKind.Pure, typeof(BlackboardGraph), "GetFloatForGraph", "entity:int", "value:float,success:bool", "p:entity,@key,>value", "success"),
        Reg("setblackboardfloat", GenericKind.Exec, typeof(BlackboardGraph), "SetFloatForGraph", "entity:int,value:float", "success:bool", "p:entity,@key,p:value", "success"),
        Reg("getblackboardint", GenericKind.Pure, typeof(BlackboardGraph), "GetIntForGraph", "entity:int", "value:int,success:bool", "p:entity,@key,>value", "success"),
        Reg("setblackboardint", GenericKind.Exec, typeof(BlackboardGraph), "SetIntForGraph", "entity:int,value:int", "success:bool", "p:entity,@key,p:value", "success"),
        Reg("getblackboardbool", GenericKind.Pure, typeof(BlackboardGraph), "GetBoolForGraph", "entity:int", "value:bool,success:bool", "p:entity,@key,>value", "success"),
        Reg("setblackboardbool", GenericKind.Exec, typeof(BlackboardGraph), "SetBoolForGraph", "entity:int,value:bool", "success:bool", "p:entity,@key,p:value", "success"),
        Reg("getblackboardentity", GenericKind.Pure, typeof(BlackboardGraph), "GetEntityForGraph", "entity:int", "value:int,success:bool", "p:entity,@key,>value", "success"),
        Reg("setblackboardentity", GenericKind.Exec, typeof(BlackboardGraph), "SetEntityForGraph", "entity:int,value:int", "success:bool", "p:entity,@key,p:value", "success"),
        Reg("getblackboardvec3", GenericKind.Pure, typeof(BlackboardGraph), "GetVec3ForGraph", "entity:int", "x:float,y:float,z:float,success:bool", "p:entity,@key,>x,>y,>z", "success"),
        Reg("setblackboardvec3", GenericKind.Exec, typeof(BlackboardGraph), "SetVec3ForGraph", "entity:int,x:float,y:float,z:float", "success:bool", "p:entity,@key,p:x,p:y,p:z", "success"),
        Reg("an_playstream", GenericKind.Exec, typeof(AudioStreamGraph), "PlayStreamForGraph", "volume:float=1,pitch:float=1,looping:bool,bus:int,fadeInSeconds:float", "voice:int,success:bool", "@sound,p:volume,p:pitch,p:looping,p:bus,p:fadeInSeconds,>voice", "success"),
        Reg("an_playstreamat", GenericKind.Exec, typeof(AudioStreamGraph), "PlayStreamAtForGraph", "x:float,y:float,z:float,volume:float=1,pitch:float=1,looping:bool,bus:int,innerCm:float=200,outerCm:float=2000,fadeInSeconds:float", "voice:int,success:bool", "@sound,p:x,p:y,p:z,p:volume,p:pitch,p:looping,p:bus,p:innerCm,p:outerCm,p:fadeInSeconds,>voice", "success"),
        Reg("an_playmusic", GenericKind.Exec, typeof(AudioStreamGraph), "PlayMusicForGraph", "volume:float=1,fadeSeconds:float=2,looping:bool=true,curve:int=1", "voice:int,success:bool", "@sound,p:volume,p:fadeSeconds,p:looping,p:curve,>voice", "success"),
        Reg("an_stopmusic", GenericKind.Exec, typeof(AudioStreamGraph), "StopMusicForGraph", "fadeSeconds:float=1", "success:bool", "p:fadeSeconds", "success"),
        Reg("an_ismusicplaying", GenericKind.Pure, typeof(AudioStreamGraph), "IsMusicPlayingForGraph", "", "playing:bool", "", "playing"),
        Reg("an_fadesound", GenericKind.Exec, typeof(AudioStreamGraph), "FadeSoundForGraph", "voice:int,targetGain:float,seconds:float=1,curve:int=1,stopWhenDone:bool", "success:bool", "p:voice,p:targetGain,p:seconds,p:curve,p:stopWhenDone", "success"),
        Reg("an_setvoiceocclusion", GenericKind.Exec, typeof(AudioStreamGraph), "SetVoiceOcclusionForGraph", "voice:int,occlusion:float", "success:bool", "p:voice,p:occlusion", "success"),
        Reg("an_setreverb", GenericKind.Exec, typeof(AudioStreamGraph), "SetReverbForGraph", "wet:float=0.3,decaySeconds:float=1.8,damping:float=0.4", "success:bool", "p:wet,p:decaySeconds,p:damping", "success"),
        Reg("an_attachaudioocclusion", GenericKind.Exec, typeof(AudioStreamGraph), "AttachAudioOcclusionForGraph", "entity:int,voice:int,rayCount:int=5,probeRadiusCm:float=50", "success:bool", "p:entity,p:voice,p:rayCount,p:probeRadiusCm", "success"),
        Reg("an_setreverbzone", GenericKind.Exec, typeof(AudioStreamGraph), "SetReverbZoneForGraph", "entity:int,halfX:float=500,halfY:float=500,halfZ:float=300,blendDistanceCm:float=200,wet:float=0.35,decaySeconds:float=1.8,damping:float=0.4,priority:int", "success:bool", "p:entity,p:halfX,p:halfY,p:halfZ,p:blendDistanceCm,p:wet,p:decaySeconds,p:damping,p:priority", "success"),
        Reg("setanimgraph", GenericKind.Exec, typeof(AnimGraphInterop), "SetAnimGraphForGraph", "entity:int,playRate:float=1", "success:bool", "p:entity,@machine,p:playRate", "success"),
        Reg("setanimparamfloat", GenericKind.Exec, typeof(AnimGraphInterop), "SetAnimParamFloatForGraph", "entity:int,value:float", "success:bool", "p:entity,@param,p:value", "success"),
        Reg("setanimparambool", GenericKind.Exec, typeof(AnimGraphInterop), "SetAnimParamBoolForGraph", "entity:int,value:bool", "success:bool", "p:entity,@param,p:value", "success"),
        Reg("triggeranim", GenericKind.Exec, typeof(AnimGraphInterop), "TriggerAnimForGraph", "entity:int", "success:bool", "p:entity,@param", "success"),
        Reg("isinanimstate", GenericKind.Pure, typeof(AnimGraphInterop), "IsInAnimStateForGraph", "entity:int", "result:bool", "p:entity,@state", "result"),
        Reg("getanimstatetime", GenericKind.Pure, typeof(AnimGraphInterop), "AnimStateTimeForGraph", "entity:int", "time:float", "p:entity", "time"),
        Reg("an_openuilayout", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "", "layout:int,success:bool", "=an_openuilayout,@path|name|text,i:0,i:0,f:0,b:0,>layout,>_,>_", "success"),
        Reg("an_closeuilayout", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "layout:int", "success:bool", "=an_closeuilayout,=,p:layout,i:0,f:0,b:0,>_,>_,>_", "success"),
        Reg("an_finduiwidget", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "layout:int", "widget:int,success:bool", "=an_finduiwidget,@path|name|text,p:layout,i:0,f:0,b:0,>widget,>_,>_", "success"),
        Reg("an_createuiwidget", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "parent:int,kind:int", "widget:int,success:bool", "=an_createuiwidget,@path|name|text,p:parent,p:kind,f:0,b:0,>widget,>_,>_", "success"),
        Reg("an_setuitext", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int", "success:bool", "=an_setuitext,@path|name|text,p:widget,i:0,f:0,b:0,>_,>_,>_", "success"),
        Reg("an_setuivalue", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int,value:float", "success:bool", "=an_setuivalue,=,p:widget,i:0,p:value,b:0,>_,>_,>_", "success"),
        Reg("an_setuichecked", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int,checked:bool", "success:bool", "=an_setuichecked,=,p:widget,i:0,f:0,p:checked,>_,>_,>_", "success"),
        Reg("an_setuivisible", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int,visible:bool=true", "success:bool", "=an_setuivisible,=,p:widget,i:0,f:0,p:visible,>_,>_,>_", "success"),
        Reg("an_setuienabled", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int,enabled:bool=true", "success:bool", "=an_setuienabled,=,p:widget,i:0,f:0,p:enabled,>_,>_,>_", "success"),
        Reg("an_getuivalue", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int", "value:float,checked:bool,selected:int,success:bool", "=an_getuivalue,=,p:widget,i:0,f:0,b:0,>selected,>value,>checked", "success"),
        Reg("an_uiwasclicked", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int", "clicked:bool", "=an_uiwasclicked,=,p:widget,i:0,f:0,b:0,>_,>_,>clicked", null),
        Reg("an_uicommandfired", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "", "fired:bool", "=an_uicommandfired,@path|name|text,i:0,i:0,f:0,b:0,>_,>_,>fired", null),
        Reg("an_setuifocus", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "widget:int", "success:bool", "=an_setuifocus,=,p:widget,i:0,f:0,b:0,>_,>_,>_", "success"),
        Reg("an_openuisettings", GenericKind.Exec, typeof(UiGraphInterop), "UiNodeForGraph", "", "layout:int,success:bool", "=an_openuisettings,=,i:0,i:0,f:0,b:0,>layout,>_,>_", "success"),
        Reg("an_spawnprefab", GenericKind.Exec, typeof(GraphPrefabs), "SpawnPrefabForGraph", "parent:int,x:float,y:float,z:float,yaw:float,scale:float=1", "entity:int", "p:parent,p:x,p:y,p:z,p:yaw,p:scale,@prefab", "entity"),
        Reg("an_destroyprefab", GenericKind.Exec, typeof(GraphPrefabs), "DestroyPrefabForGraph", "root:int", "", "p:root", null),
        Reg("an_getprefabroot", GenericKind.Exec, typeof(GraphPrefabs), "PrefabRootForGraph", "entity:int", "root:int", "p:entity", "root"),
        Reg("an_findprefabnode", GenericKind.Exec, typeof(GraphPrefabs), "FindPrefabNodeForGraph", "root:int", "entity:int", "p:root,@node?", "entity"),
        Reg("an_revertprefab", GenericKind.Exec, typeof(GraphPrefabs), "RevertPrefabForGraph", "root:int", "", "p:root", null),
        Reg("an_spawndecal", GenericKind.Exec, typeof(DecalGraph), "SpawnDecalForGraph", "x:float,y:float,z:float,nx:float,ny:float,nz:float=1,sx:float=100,sy:float=100,sz:float=100,roll:float,lifetime:float,fadeOut:float=0.5", "entity:int", "p:x,p:y,p:z,p:nx,p:ny,p:nz,p:sx,p:sy,p:sz,p:roll,p:lifetime,p:fadeOut,@base?,@normalmap?,@orm?", "entity"),
        Reg("an_cleardecals", GenericKind.Exec, typeof(DecalGraph), "ClearDecalsForGraph", "", "success:bool", "", "success"),
        Reg("an_setdecalcapacity", GenericKind.Exec, typeof(DecalGraph), "SetDecalCapacityForGraph", "capacity:int=256", "success:bool", "p:capacity", "success"),
        Reg("an_crowdsetagent", GenericKind.Exec, typeof(GraphInteropSynapseAi), "CrowdSetAgentForGraph", "entity:int,radiusCm:float=34,maxSpeedCm:float=350,maxAccelCm:float=1200,priority:float=0", "success:bool", "p:entity,p:radiusCm,p:maxSpeedCm,p:maxAccelCm,p:priority", "success"),
        Reg("an_crowdsetmode", GenericKind.Exec, typeof(GraphInteropSynapseAi), "CrowdSetModeForGraph", "entity:int,mode:int,x:float,y:float,z:float", "success:bool", "p:entity,p:mode,p:x,p:y,p:z", "success"),
        Reg("an_crowdsetbackend", GenericKind.Exec, typeof(GraphInteropSynapseAi), "CrowdSetBackendForGraph", "backend:int=0,maxAgents:int=-1", "success:bool", "p:backend,p:maxAgents", "success"),
        Reg("an_getcrowdvelocity", GenericKind.Pure, typeof(GraphInteropSynapseAi), "GetCrowdVelocityForGraph", "entity:int", "vx:float,vy:float,speed:float,success:bool", "p:entity,>vx,>vy,>speed", "success"),
        Reg("an_crowdsteer", GenericKind.Pure, typeof(GraphInteropSynapseAi), "CrowdSteerForGraph", "entity:int,dt:float,turnRate:float=360,maxSpeedCm:float=350", "forward:float,right:float,yawDelta:float,success:bool", "p:entity,p:dt,p:turnRate,p:maxSpeedCm,>forward,>right,>yawDelta", "success"),
        Reg("an_emitnoise", GenericKind.Exec, typeof(GraphInteropSynapseAi), "EmitNoiseForGraph", "x:float,y:float,z:float,loudnessCm:float=1000,tag:int=0,source:int=0", "success:bool", "p:x,p:y,p:z,p:loudnessCm,p:tag,p:source", "success"),
        Reg("an_sethearing", GenericKind.Exec, typeof(GraphInteropSynapseAi), "SetHearingForGraph", "entity:int,sensitivity:float=1,maxRangeCm:float=5000,memorySec:float=8", "success:bool", "p:entity,p:sensitivity,p:maxRangeCm,p:memorySec", "success"),
        Reg("an_getheard", GenericKind.Pure, typeof(GraphInteropSynapseAi), "GetHeardForGraph", "entity:int", "heard:bool,x:float,y:float,z:float,level:float,tag:int,confidence:float,timeSince:float,success:bool", "p:entity,>heard,>x,>y,>z,>level,>tag,>confidence,>timeSince", "success"),
        Reg("an_findcover", GenericKind.Exec, typeof(GraphInteropSynapseAi), "FindCoverForGraph", "entity:int,threatX:float,threatY:float,threatZ:float,maxSeekCm:float=2500,minThreatDistCm:float=300", "found:bool,x:float,y:float,coverId:int,success:bool", "p:entity,p:threatX,p:threatY,p:threatZ,p:maxSeekCm,p:minThreatDistCm,>found,>x,>y,>coverId", "success"),
        Reg("an_releasecover", GenericKind.Exec, typeof(GraphInteropSynapseAi), "ReleaseCoverForGraph", "entity:int", "success:bool", "p:entity", "success"),
        Reg("an_iscovered", GenericKind.Pure, typeof(GraphInteropSynapseAi), "IsCoveredForGraph", "entity:int,threatX:float,threatY:float,threatZ:float", "covered:bool", "p:entity,p:threatX,p:threatY,p:threatZ", "covered"),
        Reg("an_squadjoin", GenericKind.Exec, typeof(GraphInteropSynapseAi), "SquadJoinForGraph", "entity:int,squadId:int=0,spacingCm:float=200", "success:bool", "p:entity,p:squadId,p:spacingCm", "success"),
        Reg("an_squadsettarget", GenericKind.Exec, typeof(GraphInteropSynapseAi), "SquadSetTargetForGraph", "squadId:int=0,x:float,y:float,z:float", "success:bool", "p:squadId,p:x,p:y,p:z", "success"),
        Reg("an_getsquadslot", GenericKind.Pure, typeof(GraphInteropSynapseAi), "GetSquadSlotForGraph", "entity:int", "role:int,x:float,y:float,z:float,success:bool", "p:entity,>role,>x,>y,>z", "success"),
    };
}
