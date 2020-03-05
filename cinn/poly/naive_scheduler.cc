#include "cinn/poly/naive_scheduler.h"

#include <vector>

namespace cinn {
namespace poly {

void NaiveScheduler::PartitionGroups() {
  // treat each node as a unique group, collect the groups in topological order.
  std::vector<common::GraphNode *> nodes_in_order;
  std::vector<common::GraphEdge *> edges_in_order;
  std::tie(nodes_in_order, edges_in_order) = graph_->topological_order();
  for (auto *node : nodes_in_order) {
    detail::Group group({Shared<poly::DataFlowGraphNode>(node->As<poly::DataFlowGraphNode>())});
    groups_.emplace_back(std::move(group));
  }
}

}  // namespace poly
}  // namespace cinn
