// Revision-control panel: git status/log/diff caches and selection.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct RevisionControlState {
    bool                showRevisionControl_ = false;   // Window > Revision Control.
    std::string         rcProjectDir_;        // Project the latched answer describes.
    std::string         rcRoot_;              // Repository root, git spelling.
    // rcRoot_ normalized (lower-case, forward slashes, no trailing separator). Precomputed per latch.
    std::string         rcRootKey_;
    editor::RepoStatus  rcStatus_;
    std::string         rcWhy_;
    bool                rcAnswered_ = false;  // Query completed for rcProjectDir_ at least once.
    bool                rcGitPresent_ = false;
    f64                 rcRefreshedAt_ = -1.0;  // Last latch time (-1 = never).
    // Badge table (one entry per changed path, sorted for binary search).
    std::vector<std::pair<std::string, editor::FileStatus>> rcMarks_;
    // Row the panel is showing history/diff for.
    std::string         rcSelected_;
    std::vector<editor::LogEntry> rcLog_;
    std::vector<std::string>      rcDiff_;
    std::string         rcLogWhy_, rcDiffWhy_;
    bool                rcSelectedDiffable_ = false;   // Text asset that diff viewer can show.
    // Which diff side is on screen (file can have both: staged + edited again).
    editor::DiffSide    rcDiffSide_ = editor::DiffSide::Worktree;
};
