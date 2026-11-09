#pragma once
// Window > Data Layers (WAVE2_UE W10 step 4): the widgets over DataLayerModel.hpp. Every rule is in the model; this draws the layer tree
// (eye and load toggles, name, member count, initial state, runtime flag) and the buttons, and tells the host what to do through DataLayersHost.
// The host (SandboxApp, SandboxDataLayers.cpp) owns the model, the undo stack and the selection.
#include "DataLayerModel.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::editor {

struct DataLayersHost {
    std::function<std::vector<dl::Key>()> selection;               // keys of the selected entities
    std::function<void(const std::vector<dl::Key>&)> selectKeys;   // make these the selection
    std::function<void(dl::Edit)> push;                            // the edit is already applied: record it for undo, mark the level dirty
    std::function<void()> refreshVisibility;                       // a visible / loaded toggle changed
};

struct DataLayersUi {
    std::string selected;       // the layer the buttons act on
    std::string renaming;       // the layer whose name is being typed
    char nameBuf[128] = {};
    bool focusName = false;
};

// Call every frame; draws nothing while *open is false.
void drawDataLayersPanel(bool* open, dl::Model& model, DataLayersUi& ui, const DataLayersHost& host, f32 dpi);

}  // namespace aver::editor
