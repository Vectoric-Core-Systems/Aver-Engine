#pragma once
// Private to Aver.Framework: the custom-program registry's answer for the fog volume pack (EnvFxAbi.cpp).
#include <cstdint>
#include <string>

// 1-based position of the fog program `name` among the registered fog programs (the renderer's table order), 0 when it is not registered or past the renderer's table.
uint32_t aver_fw_cenv_fog_index(const std::string& name);
