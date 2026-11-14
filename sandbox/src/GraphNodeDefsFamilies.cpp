// The node catalog's family half (GraphNodeDefs<Family>.hpp, listed by the generated GraphNodeFamilies.gen.inc).
#include "GraphNodeDefs.hpp"

#if !__has_include("GraphNodeFamilies.gen.inc")
#error "GraphNodeFamilies.gen.inc is missing: re-run cmake (cmake/GraphNodeFamilies.cmake writes it)"
#endif

namespace aver::editor::detail {

#include "GraphNodeFamilies.gen.inc"

void appendFamilyCatalog(std::vector<GraphNodeDesc>& t) { appendGeneratedFamilies(t); }

} // namespace aver::editor::detail
