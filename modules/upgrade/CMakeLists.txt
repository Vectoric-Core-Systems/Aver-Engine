# Aver.Upgrade -- the project migration chain (see include/aver/upgrade/Upgrade.hpp).
#
# Depends on Core and nothing else. Deliberately NOT on Aver.Formats: a step edits files on a path
# it was handed, and the manifest itself is written by whoever owns the format. Keeping this module
# ignorant of the manifest is what lets a step be frozen at its shipping form -- it cannot drift
# with a struct it never sees.
aver_add_module(Aver.Upgrade
  SOURCES src/Upgrade.cpp
  DEPS    Aver.Core
  PUBLIC_DEFS AVER_MODULE_UPGRADE=1)
