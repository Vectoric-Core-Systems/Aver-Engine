// MCP automation bridge: port, pointer injection, graph-debug targets.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct McpState {
    // A screenshot asked for over MCP: captureCheck requests a new capture for it instead of re-reading frame sf's.
    bool mcpShotRequest_=false;
#if AVER_MODULE_MCP
    u16  mcpPort_ = 0;           // 0 = never asked for.
#if AVER_MODULE_SCENE && AVER_MODULE_SCRIPTING
    // Graph ABI state (load/attach/tick). One graph at a time.
    std::string  mcpGraphPath_;
    scene::Entity mcpGraphEntity_ = scene::kInvalidEntity;
    f32          mcpGraphTimeSeconds_ = 0.0f;
#endif
    f32 mcpPointerX_ = 0.0f, mcpPointerY_ = 0.0f;
    int mcpPointerHold_ = 0;   // frames left
    void* mcpHookCtx_ = nullptr;   // the ImGui context holdMcpPointer installed its NewFramePre hook on
    bool mcpAbisRegistered_ = false;
#endif
};
