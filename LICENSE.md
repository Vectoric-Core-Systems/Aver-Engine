AVER ENGINE — SOURCE CODE EVALUATION LICENSE AGREEMENT

Copyright (c) 2026 Hydrogen-Isotope. All Rights Reserved.
Developed by Vectoric-Core-Systems.
Source: https://github.com/Vectoric-Core-Systems/Aver-Source

This Evaluation License Agreement ("Agreement") is a legal agreement between you 
("Licensee" or "Tester") and Hydrogen-Isotope, developing as Vectoric-Core-Systems
("Licensor"), for access to the 
private source code, binaries, and related documentation for the Aver Engine ("Software").

BY CLONING, DOWNLOADING, COMPILING, OR USING THIS SOFTWARE, YOU AGREE TO BE BOUND BY 
THE TERMS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT ACCESS OR USE THE SOFTWARE.

1. GRANT OF EVALUATION LICENSE
Subject to the terms of this Agreement, Licensor grants Licensee a non-exclusive, 
non-transferable, non-sublicensable, revocable license to:
  a) Download, compile, and run the Software solely on hardware owned or controlled by Licensee.
  b) Use the Software strictly for personal evaluation, testing, debugging, and providing feedback.

2. RESTRICTIONS
Licensee shall NOT:
  a) Distribute, share, publish, lease, sell, sublicense, or otherwise transfer the Software, 
     in whole or in part, to any third party.
  b) Make the repository public or re-host the source code on any public version control service.
  c) Use the Software or any derivative work to develop, build, or ship any commercial game, 
     application, or service without a separate written license from Licensor.
  d) Remove, alter, or obscure any copyright, trademark, or proprietary rights notices.

3. CONFIDENTIALITY & NON-DISCLOSURE
  a) The Software, including its source code, architecture, and design, is the confidential and 
     proprietary information of Licensor.
  b) Licensee agrees to hold the Software in strict confidence and prevent unauthorized disclosure, 
     except that Licensee may share public media (e.g., screenshots, short video clips of execution) 
     strictly with the prior written or explicit consent of Licensor.

4. OWNERSHIP & FEEDBACK
  a) Licensor retains all right, title, and interest in and to the Software, including all 
     intellectual property rights therein.
  b) Any feedback, bug reports, performance metrics, or feature suggestions ("Feedback") 
     provided by Licensee to Licensor may be used by Licensor without restriction, royalty, 
     or obligation to Licensee.

5. TERMINATION
This license is effective until terminated. Licensor may terminate this Agreement at any time 
with or without cause. Upon termination, Licensee must immediately cease all use of the Software 
and delete all local copies of the repository and binaries.

6. NO WARRANTY (AS-IS)
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING 
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND 
NONINFRINGEMENT. IN NO EVENT SHALL THE LICENSOR BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER 
LIABILITY ARISING FROM THE USE OF THE SOFTWARE.

THIRD-PARTY COMPONENTS

This Software incorporates third-party components that remain under their own licences and
are NOT covered by the grant above. Each carries its own licence text in its directory:

  third_party/imgui             Dear ImGui                 MIT
  third_party/stb               stb single-file libraries  Public domain / MIT
  third_party/meshoptimizer     meshoptimizer              MIT
  third_party/fonts             Roboto                     Apache-2.0
  third_party/fonts             Material Icons             Apache-2.0
  third_party/vulkan-headers    Vulkan-Headers (Khronos)   Apache-2.0
  third_party/fidelityfx-fsr    AMD FidelityFX FSR 1       MIT
  third_party/fidelityfx-denoiser  AMD FidelityFX Denoiser MIT
  modules/physics.jolt/Jolt     Jolt Physics               MIT
  third_party/dxc-spirv         DirectX Shader Compiler    MIT + LLVM Release Licence
                                (redistributed binary: dxcompiler.dll)  (Univ. of Illinois/NCSA)
  third_party/nuget             .NET Compiler Platform (Roslyn)         MIT
  third_party/nrd               NVIDIA Real-Time Denoisers (NRD)    NVIDIA RTX SDKs Licence
  third_party/rtxdi             NVIDIA RTXDI (ReSTIR DI/GI)         NVIDIA RTX SDKs Licence
  third_party/rtxgi             NVIDIA RTXGI (SHaRC)                NVIDIA RTX SDKs Licence
  third_party/mathlib           NVIDIA MathLib                      MIT
  third_party/shadermake        NVIDIA ShaderMake                   MIT

Nothing in this Agreement restricts rights granted to you by those licences, and no per-file
copyright notice in this repository is applied to them.


NVIDIA SOFTWARE DEVELOPMENT KITS

This software contains source code provided by NVIDIA Corporation.

That sentence is not a courtesy. It is the notice the NVIDIA RTX SDKs Licence requires, in the
words it requires, and it is reproduced here because this product contains and distributes NVIDIA
SDK code:

  * NRD (NVIDIA Real-Time Denoisers) is compiled and statically linked into both the editor and
    the packaged-game runtime. It denoises the ray-traced global-illumination signal.

  * RTXDI's shader source is distributed VERBATIM. Its HLSL is copied whole into the shaders
    directory of every build and every package, and the engine's own shaders include it -- the
    ReSTIR GI estimator is RTXDI's. This is NVIDIA source code shipped as readable text, not
    merely linked as a compiled artefact, which is the plainest possible case for the notice above.

  * MathLib and ShaderMake are NVIDIA-authored and MIT-licensed. Their own copyright and permission
    notices are reproduced in their directories and travel with them.

  * RTXGI's SHaRC headers are distributed VERBATIM: they are copied into the shaders directory of
    every build and every package, although no engine shader includes them yet. RTXGI is not
    built or linked. The terms below apply to it as they do to RTXDI.

Packaged builds carry this same notice, the list of NVIDIA SDKs they use, and each SDK's licence in
full, in THIRD-PARTY-NOTICES.txt (written by scripts/NvidiaNotices.ps1).

WHAT NVIDIA REQUIRES. The full text governs and is reproduced unmodified in each directory
(third_party/nrd/LICENSE.txt, third_party/rtxdi/LICENSE.txt, third_party/rtxgi/License.md). The
obligations that bear on anyone distributing this software are:

  1. The notice at the head of this section must be included in modifications and derivative works
     of source code distributed.

  2. An application must have material additional functionality beyond the included portions of
     the SDK.

  3. Attribution. Where an application has a CREDIT SCREEN, use of the applicable SDK must be
     attributed and the NVIDIA Marks included on it; where no credit screen is present, attribution
     goes prominently in end-user documentation instead.

     THIS PRODUCT HAS NO CREDIT SCREEN, AND THIS DOCUMENT IS THE ATTRIBUTION. That is a deliberate
     choice, not an omission: a credit screen would engage the NVIDIA Marks requirement, and Mark
     usage is separately gated by NVIDIA on style, colour and typeface specifications plus prior
     written approval of a sample use -- approval that has not been sought and is not needed on
     this route.

     NO NVIDIA MARKS ARE USED BY THIS PRODUCT. The NVIDIA name appears here as plain text
     identifying the SDKs in use. NVIDIA's trademarks, logos and brand assets are not reproduced
     and are not licensed by this notice.

     A DIFFERENT CLAUSE NAMES SPLASH SCREENS AND ABOUT BOXES, and it does not apply here: clause
     6.1(b) of the Supplement governs applications incorporating the DLSS or NGX SDK, neither of
     which is included in this product. Clause 6.1(c) is the one that governs the SDKs actually
     used, and it speaks only of a credit screen or, failing that, end-user documentation.

  4. Onward distribution must be subject to terms at least as protective as NVIDIA's own,
     including the licence grant, the restrictions, and the protection of NVIDIA's intellectual
     property rights.

  5. Interoperability. Applications incorporating the SDK must be fully interoperable with
     compatible GPU hardware products designed by NVIDIA or its affiliates. This engine reaches
     these SDKs only through its vendor-agnostic RHI and never through a backend-specific path,
     and NRD is built with whichever offline shader compiler is present rather than an
     NVIDIA-specific toolchain.

  6. NVIDIA reserves the right to identify licensees publicly, and requires notification prior to
     commercial release of applications incorporating the DLSS or NGX SDKs specifically.
     NEITHER THE DLSS SDK NOR THE NGX SDK IS INCLUDED IN THIS SOFTWARE, so that notification
     obligation does not arise. The DLSS-specific and NGX-specific terms in NVIDIA's licence --
     the NVIDIA-GPU-only development restriction, the cloud-service limitation, and the
     over-the-air update terms -- likewise do not apply to the SDKs used here.

Nothing in this Agreement grants you any right in NVIDIA's software or intellectual property.
Your rights in the NVIDIA components are those NVIDIA's own licence gives you, and no more.
