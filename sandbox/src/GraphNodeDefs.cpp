// The node catalog's one definition (see graphNodeCatalog in GraphNodeDefs.hpp for why it is not inline).
#include "GraphNodeDefs.hpp"

namespace aver::editor {

const std::vector<GraphNodeDesc>& graphNodeCatalog() {
    static const std::vector<GraphNodeDesc> table = detail::buildCatalog();
    return table;
}

} // namespace aver::editor
