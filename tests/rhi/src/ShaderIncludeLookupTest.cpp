// ShaderIncludeLookupTest -- rhi::shaderIncludeText, the one include lookup both compilers use (DXC's DxcShaderInclude
// and D3D12's FXC fallback), with no device: the files sit in a temp directory given to setShaderSourceDir.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) AVER_INFO("   PASS  {}", what);
    else      { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static void write(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << text;
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "AverShaderIncludeLookupTest";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    // Names no deployed shader uses, so the executable's own shaders/ folder (searched second) cannot answer.
    write(dir / "aver_lookup_top.hlsli", "// top\r\n");
    write(dir / "sub" / "deep" / "aver_lookup_deep.hlsli", "// deep\n");
    write(dir / "a" / "aver_lookup_same.hlsli", "// a\n");
    write(dir / "b" / "aver_lookup_same.hlsli", "// b\n");
    rhi::setShaderSourceDir(dir.string());

    check(rhi::shaderIncludeText("aver_lookup_top.hlsli") == "// top\n", "a bare name resolves, CRLF normalised");
    check(rhi::shaderIncludeText("./aver_lookup_top.hlsli") == "// top\n", "a leading ./ is stripped (DXC's bare-include shape)");
    check(rhi::shaderIncludeText("sub/deep/aver_lookup_deep.hlsli") == "// deep\n", "a subdirectory path resolves");
    check(rhi::shaderIncludeText("sub/deep/sub/deep/aver_lookup_deep.hlsli") == "// deep\n",
          "an includer's directory prepended to a rooted include still resolves");
    check(rhi::shaderIncludeText("prefix/sub/deep/aver_lookup_deep.hlsli") == "// deep\n", "an unknown leading directory is dropped");
    check(rhi::shaderIncludeText(".\\sub\\deep\\aver_lookup_deep.hlsli") == "// deep\n", "backslashes (FXC's shape on Windows) resolve");
    check(rhi::shaderIncludeText("a/aver_lookup_same.hlsli") == "// a\n" &&
          rhi::shaderIncludeText("b/aver_lookup_same.hlsli") == "// b\n",
          "two files with one name in different folders stay distinct");
    check(rhi::shaderIncludeText("aver_lookup_missing.hlsli").empty(), "a missing include is empty (logged), not some other file");

    rhi::setShaderSourceDir("");
    std::filesystem::remove_all(dir, ec);
    AVER_INFO(g_failures ? "ShaderIncludeLookupTest: {} FAILURES" : "ShaderIncludeLookupTest: all checks passed ({})", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
