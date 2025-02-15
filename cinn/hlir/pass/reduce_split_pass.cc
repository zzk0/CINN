// Copyright (c) 2022 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cinn/common/graph_utils.h"
#include "cinn/hlir/framework/graph.h"
#include "cinn/hlir/framework/pass.h"
#include "cinn/hlir/pass/infershape.h"

namespace cinn {
namespace hlir {
namespace pass {
namespace {

using common::GraphNode;
using framework::Node;
using framework::NodeData;
using framework::Operator;
using framework::shape_t;

bool IsReduceOp(const framework::Node* node) {
  static std::unordered_set<std::string> reduce_op_type = {
      "reduce_sum", "reduce_mean", "reduce_max", "reduce_min", "reduce_all", "reduce_any"};
  if (reduce_op_type.count(node->op()->name)) {
    return true;
  } else {
    return false;
  }
}

std::pair<int, int> DivideToClosetNum(int n) {
  int a = sqrt(n);
  int b = n / a;
  while (a * b != n) {
    if (a * b < n) {
      a++;
      b = n / a;
    } else {
      a--;
      b = n / a;
    }
  }
  return {a, b};
}

uint32_t NextPowerOf2(uint32_t n) {
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n++;
}

class ReduceSplitPass {
 public:
  // Find the reduce op with nwhc format and large shape, split it into two ops
  static int Apply(framework::Graph* graph) {
    int MAX_NUM_THREADS               = common::DefaultNVGPUTarget().max_num_threads();
    constexpr int MAX_ITER_PER_THREAD = 32;  // empirical value

    int cnt          = 0;
    auto& shape_dict = graph->GetMutableAttrs<absl::flat_hash_map<std::string, shape_t>>("infershape");
    auto& dtype_dict = graph->GetMutableAttrs<absl::flat_hash_map<std::string, Type>>("inferdtype");

    // loop the nodes in graph and find reduce_xx op
    auto nodes_inorder = std::get<0>(graph->topological_order());
    for (auto node : nodes_inorder) {
      if (!node->safe_as<Node>()) {
        continue;
      }
      auto n = node->safe_as<Node>();
      if (IsReduceOp(n)) {
        auto* op  = n->op();
        auto name = op->name;

        auto dims     = absl::get<std::vector<int>>(n->attrs.attr_store.at("dim"));
        bool keep_dim = absl::get<bool>(n->attrs.attr_store.at("keep_dim"));
        auto in       = (*n->inlinks().begin())->source()->safe_as<NodeData>();
        auto out      = (*n->outlinks().begin())->sink()->safe_as<NodeData>();

        auto in_shape  = shape_dict.at(in->id());
        auto out_shape = shape_dict.at(out->id());
        // all preceding reduced
        CHECK(in_shape.size() > 1);
        // [NHWC]->[C], only the last dim kept
        bool all_preceding_dim_reduced = true;
        for (auto i = 0; i < in_shape.size() - 1; ++i) {
          if (std::find(dims.begin(), dims.end(), i) == dims.end()) {
            all_preceding_dim_reduced = false;
          }
        }
        int numel        = std::accumulate(in_shape.begin(), in_shape.end(), 1, std::multiplies<int>());
        int reduce_numel = std::accumulate(in_shape.begin(), in_shape.end() - 1, 1, std::multiplies<int>());
        CHECK(reduce_numel > 0);
        VLOG(4) << "numel: " << numel << ", reduce_numel: " << reduce_numel << ", MAX_NUM_THREADS: " << MAX_NUM_THREADS
                << ", MAX_ITER_PER_THREAD: " << MAX_ITER_PER_THREAD;
        // if the numel is not large enough, it is no need to split
        if ((!all_preceding_dim_reduced) || numel <= MAX_NUM_THREADS * MAX_ITER_PER_THREAD) {
          continue;
        }

        auto res          = DivideToClosetNum(reduce_numel);
        int reduce_numel0 = std::get<0>(res), reduce_numel1 = std::get<1>(res);
        VLOG(4) << "reduce_numel0: " << reduce_numel0 << " reduce_numel1: " << reduce_numel1;

        // create reshape node0
        Node* reshape0 = new Node(Operator::Get("reshape"), "reshape", common::UniqName("reshape_split"));
        reshape0->attrs.attr_store["shape"] =
            std::vector<int>{reduce_numel0, reduce_numel1, in_shape[in_shape.size() - 1]};
        graph->RegisterNode(reshape0->id(), reshape0);
        in->LinkTo(reshape0);
        in->UnLinkSingleTo(node);
        node->UnLinkSingleTo(out);
        auto reshape0_data = new NodeData(Shared<Node>(reshape0), 0, 0, common::UniqName("var"), false);
        graph->RegisterNode(reshape0_data->id(), reshape0_data);
        reshape0->LinkTo(reshape0_data);
        shape_dict[reshape0_data->id()] = absl::get<std::vector<int>>(reshape0->attrs.attr_store.at("shape"));
        dtype_dict[reshape0_data->id()] = common::Str2Type(common::Type2Str(dtype_dict[in->id()]));

        // create reduce node0
        Node* reduce0                         = new Node(Operator::Get(name), name, common::UniqName(name + "_split"));
        reduce0->attrs.attr_store["dim"]      = std::vector<int>{0};
        reduce0->attrs.attr_store["keep_dim"] = absl::get<bool>(n->attrs.attr_store.at("keep_dim"));
        graph->RegisterNode(reduce0->id(), reduce0);
        reshape0_data->LinkTo(reduce0);
        auto reduce0_data = new NodeData(Shared<Node>(reduce0), 0, 0, common::UniqName("var"), false);
        graph->RegisterNode(reduce0_data->id(), reduce0_data);
        reduce0->LinkTo(reduce0_data);
        shape_dict[reduce0_data->id()] = keep_dim ? std::vector<int>{1, reduce_numel1, in_shape[in_shape.size() - 1]}
                                                  : std::vector<int>{reduce_numel1, in_shape[in_shape.size() - 1]};
        dtype_dict[reduce0_data->id()] = common::Str2Type(common::Type2Str(dtype_dict[in->id()]));

        // create reduce node1
        Node* reduce1                         = new Node(Operator::Get(name), name, common::UniqName(name + "_split"));
        reduce1->attrs.attr_store["dim"]      = keep_dim ? std::vector<int>{0, 1} : std::vector<int>{0};
        reduce1->attrs.attr_store["keep_dim"] = absl::get<bool>(n->attrs.attr_store.at("keep_dim"));
        graph->RegisterNode(reduce1->id(), reduce1);
        reduce0_data->LinkTo(reduce1);
        auto reduce1_data = new NodeData(Shared<Node>(reduce1), 0, 0, common::UniqName("var"), false);
        graph->RegisterNode(reduce1_data->id(), reduce1_data);
        reduce1->LinkTo(reduce1_data);
        shape_dict[reduce1_data->id()] = keep_dim ? std::vector<int>{1, 1, in_shape[in_shape.size() - 1]}
                                                  : std::vector<int>{in_shape[in_shape.size() - 1]};
        dtype_dict[reduce1_data->id()] = common::Str2Type(common::Type2Str(dtype_dict[in->id()]));

        // create reshape node1
        Node* reshape1 = new Node(Operator::Get("reshape"), "reshape", common::UniqName("reshape_split"));
        reshape1->attrs.attr_store["shape"] = out_shape;
        graph->RegisterNode(reshape1->id(), reshape1);
        reduce1_data->LinkTo(reshape1);
        reshape1->LinkTo(out);
        out->source_node = common::Shared<Node>(reshape1);

        // drop old node
        graph->DropNode(node);

        cnt++;
      }
    }
    return cnt;
  }

 private:
};

}  // namespace

void ReduceSplitFunc(framework::Graph* graph) {
  VLOG(3) << "Before ReduceSplitPass:\n" << graph->DebugGroupedGraph(std::unordered_set<std::string>{}) << std::endl;
  int n = ReduceSplitPass::Apply(graph);
  VLOG(3) << "ReduceSplit was performed " << n << " times.";
  VLOG(3) << "After ReduceSplitPass:\n" << graph->DebugGroupedGraph(std::unordered_set<std::string>{}) << std::endl;
}

}  // namespace pass
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(ReduceSplit) {
  CINN_REGISTER_PASS(ReduceSplit)
      .describe("")
      .set_change_structure(true)
      .provide_graph_attr("infershape")
      .provide_graph_attr("inferdtype")
      .set_body(cinn::hlir::pass::ReduceSplitFunc);
  return true;
}
