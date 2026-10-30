#pragma once
// Whether log lines go to stdout/stderr (the default). A library with its own copy of Core, such as an extension, turns it off and forwards
// every line through a sink (setLogSink) to its host, which writes each line once.

namespace aver {

void setLogConsole(bool enabled);

} // namespace aver
