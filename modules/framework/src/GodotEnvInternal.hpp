#pragma once
// Private to Aver.Framework: the converted-shader registry's answer for the fog volume pack (EnvFxAbi.cpp).
#include <cstdint>
#include <string>

// 1-based position of the fog shader `name` among the registered fog shaders (the renderer's table order), 0 when it is not registered or past the renderer's table.
uint32_t aver_fw_genv_fog_index(const std::string& name);
