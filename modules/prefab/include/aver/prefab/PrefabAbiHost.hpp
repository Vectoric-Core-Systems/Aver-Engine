#pragma once
// The host half of prefab_abi.h: an AverPrefabHost whose function pointers drive a PrefabSystem.
// The host installs it once, after creating its system:
//     const AverPrefabHost h = aver::prefab::makeAbiHost(sys);
//     aver_prefab_set_host(&h);                                         // from Aver.Prefab.Abi
#include "aver/prefab/PrefabSystem.hpp"
#include "aver/prefab/prefab_abi.h"

namespace aver::prefab {

// `sys` must outlive the host table; the pointers capture its address.
AverPrefabHost makeAbiHost(PrefabSystem& sys);

} // namespace aver::prefab
