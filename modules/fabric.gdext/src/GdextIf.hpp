#pragma once
#include "GdextHost.hpp"

namespace aver::gdext {

// The class, object, signal and registration half of the interface table (GdextClassDb.cpp).
GDExtensionInterfaceFunctionPtr classDbFunction(const char* name);

// The flat form of a Variant on the C ABI (GdextWire.cpp). `wireDecode` fills an uninitialised Variant and advances `p`.
void wireEncode(const Var* v, std::vector<unsigned char>& out);
bool wireDecode(const unsigned char*& p, const unsigned char* end, Var* out);

} // namespace aver::gdext
