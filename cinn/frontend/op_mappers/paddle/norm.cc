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

#include "cinn/frontend/op_mapper_registry.h"
#include "cinn/frontend/op_mappers/common_utils.h"

namespace cinn {
namespace frontend {
namespace paddle_mappers {

void NormOpMapper(const paddle::cpp::OpDesc& op_desc, const OpMapperContext& ctx) {
  CHECK_EQ(op_desc.Input("X").size(), 1UL);
  auto x_name = op_desc.Input("X").front();
  CHECK_EQ(op_desc.Output("Out").size(), 1UL);
  auto out_name = op_desc.Output("Out").front();
  auto x        = ctx.GetVar(x_name);

  CHECK(op_desc.HasAttr("axis"));
  CHECK(op_desc.HasAttr("epsilon"));
  auto axis    = op_desc.GetAttr<int32_t>("axis");
  auto epsilon = op_desc.GetAttr<float>("epsilon");
  auto out     = ctx.Builder()->Norm(x, axis, epsilon);

  ctx.AddVar(out_name, out);
  ctx.AddVarModelToProgram(out_name, out->id);
}

}  // namespace paddle_mappers
}  // namespace frontend
}  // namespace cinn

CINN_REGISTER_HELPER(paddle_norm) {
  CINN_REGISTER_OP_MAPPER(norm, cinn::frontend::paddle_mappers::NormOpMapper)
  return true;
}
