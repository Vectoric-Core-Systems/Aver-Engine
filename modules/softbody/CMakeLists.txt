# Aver.SoftBody -- the plastic half of soft bodies: permanent set, yield, break and tear.
#
# Core ONLY, by construction. The solver is arithmetic on particles and distance constraints, so the
# whole thing (including the async worker) is decidable on a machine with no GPU, no scene and no
# Jolt -- tests/softbody links nothing else. Jolt keeps the ELASTIC path (aver_phys_softbody_*);
# nothing here includes or links Aver.Physics, and the two never share state.
#
# NOT in the default module list until wired: see docs/SOFTBODY.md section "Wiring".
aver_add_module(Aver.SoftBody
  SOURCES
    src/Cage.cpp
    src/Solver.cpp
    src/RenderBinding.cpp
    src/AsyncSolver.cpp
    src/softbody_abi.cpp
  DEPS
    Aver.Core
)

target_compile_definitions(Aver.SoftBody PUBLIC AVER_MODULE_SOFTBODY=1)

if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(Aver.SoftBody PUBLIC Threads::Threads)
endif()
