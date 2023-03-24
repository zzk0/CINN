// Copyright (c) 2023 CINN Authors. All Rights Reserved.
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

#include "cinn/frontend/op_mapper_registry.h"
#include "cinn/frontend/op_mappers/common_utils.h"

namespace cinn {
namespace frontend {
namespace paddle_mappers {

void ScatterOpMapper(const paddle::cpp::OpDesc& op_desc, const OpMapperContext& ctx) {
  CHECK_EQ(op_desc.Input("X").size(), 1UL);
  auto x_name = op_desc.Input("X").front();
  CHECK_EQ(op_desc.Input("Ids").size(), 1UL);
  auto ids_name = op_desc.Input("Ids").front();
  CHECK_EQ(op_desc.Input("Updates").size(), 1UL);
  auto updates_name = op_desc.Input("Updates").front();
  CHECK_EQ(op_desc.Output("Out").size(), 1UL);
  auto out_name = op_desc.Output("Out").front();

  auto overwrite = utils::GetAttrOrDefault<bool>(op_desc, "overwrite", true);
  VLOG(4) << "out_name = scatter(X=" << x_name << ", Ids=" << ids_name << ", Updates=" << updates_name
          << ", overwrite=" << overwrite << ")";

  auto input   = ctx.GetVar(x_name);
  auto indices = ctx.GetVar(ids_name);
  auto updates = ctx.GetVar(updates_name);
  Variable out;
  if (overwrite) {
    out = ctx.Builder()->ScatterAssign(input, updates, indices);
  } else {
    auto zeros = ctx.Builder()->FillConstant(
        updates->shape, 0, common::UniqName("scatter_zeros"), common::Type2Str(updates->type));
    out = ctx.Builder()->ScatterAssign(input, zeros, indices);
    out = ctx.Builder()->ScatterAdd(out, updates, indices);
  }

  ctx.AddVar(out_name, out);
  ctx.AddVarModelToProgram(out_name, out->id);
}

}  // namespace paddle_mappers
}  // namespace frontend
}  // namespace cinn

CINN_REGISTER_HELPER(paddle_scatter) {
  CINN_REGISTER_OP_MAPPER(scatter, cinn::frontend::paddle_mappers::ScatterOpMapper)
  return true;
}
