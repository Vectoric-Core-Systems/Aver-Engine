#include "aver/platform/Console.hpp"

namespace aver::platform {

// Nothing to adopt. A POSIX process inherits its parent's stdin/stdout/stderr whatever it is linked
// as, so a run from a terminal already prints there and a run from a desktop launcher already goes
// wherever the launcher sends it. The Win32 subsystem split this function exists to undo does not
// exist here.
void attachParentConsole() {}

}  // namespace aver::platform
