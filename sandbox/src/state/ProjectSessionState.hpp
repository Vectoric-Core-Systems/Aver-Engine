// Open project/level identity and pending open requests: paths, save status, deep-link asset/entity/graph-node targets.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct ProjectSessionState {
    std::string projectSaveStatus_;
    std::string saveLevelTo_;         // --save-level <out>
    std::string bakeLevelPackAsset_;  // --bake-level-pack <level>  [wave3/level-instances]
    std::string windowTitleShown_;
    // THE OPEN LEVEL'S FILE PATH AND DISPLAY NAME, outside the scene block they were declared in.
    // Both are plain strings about a file on disk. loadNavForLevel reads levelPath_ to find the
    // .ocnav baked beside it, and that function is guarded on AVER_MODULE_SYNAPSE -- the grid math,
    // which exists in a tree with no entity world at all. A path is not scene state even when
    // everything that path loads INTO is.
    std::string levelPath_, levelName_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    std::string openMapPath_;        // <path>.ocmap given on the command line, if any
    std::string openAsset_;
    std::string selectEntity_;   // --select
    std::string lastOpenAssetPath_;  // last path --open-asset opened; openAsset_ itself is cleared once consumed.
    std::string graphSelectNode_;
    std::string graphTab_; // --graph-select <nodeId>; see setGraphSelectNode's own comment
    editor::ProjectUpgrade pendingUpgrade_;
    // Frames the project loading screen has been up (capped in onRender).
    int projectLoadingFrames_ = 0;
};
