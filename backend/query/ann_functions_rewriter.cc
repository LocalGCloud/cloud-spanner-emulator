//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/query/ann_functions_rewriter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/function_signature.h"
#include "googlesql/public/json_value.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "common/errors.h"
#include "third_party/spanner_pg/datatypes/extended/pg_jsonb_type.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

absl::Status ValidateOptions(googlesql::JSONValueConstRef options,
                             const std::string& function_name) {
  if (!options.IsObject() || !options.HasMember("num_leaves_to_search") ||
      !options.GetMember("num_leaves_to_search").IsUInt64()) {
    return error::ApproxDistanceFunctionInvalidJsonOption(function_name);
  }
  return absl::OkStatus();
}

}  // namespace

bool IsANNFunction(std::string function_name) {
  if (function_name == "approx_cosine_distance" ||
      function_name == "approx_dot_product" ||
      function_name == "approx_euclidean_distance") {
    return true;
  }
  return false;
}

absl::Status ANNFunctionsRewriter::VisitResolvedFunctionCall(
    const googlesql::ResolvedFunctionCall* node) {
  if (!IsANNFunction(node->function()->Name())) {
    return CopyVisitResolvedFunctionCall(node);
  }
  if (node->argument_list_size() < 3) {
    return error::ApproxDistanceFunctionOptionsRequired(
        node->function()->Name());
  }
  GOOGLESQL_RET_CHECK(node->argument_list_size() == 3);
  const googlesql::ResolvedExpr* argument_for_placeholder =
      node->argument_list().back().get();
  if (!argument_for_placeholder->Is<googlesql::ResolvedLiteral>()) {
    return error::ApproxDistanceFunctionOptionMustBeLiteral(
        node->function()->Name());
  }
  const googlesql::Value& placeholder_value =
      argument_for_placeholder->GetAs<googlesql::ResolvedLiteral>()->value();
  GOOGLESQL_RET_CHECK(placeholder_value.has_content());

  if (placeholder_value.type_kind() == googlesql::TYPE_JSON) {
    GOOGLESQL_RETURN_IF_ERROR(ValidateOptions(placeholder_value.json_value(),
                                    node->function()->Name()));
  } else if (placeholder_value.type() ==
             postgres_translator::spangres::datatypes::GetPgJsonbType()) {
    // PostgreSQL passes the options as JSONB.
    GOOGLESQL_ASSIGN_OR_RETURN(
        absl::Cord jsonb,
        postgres_translator::spangres::datatypes::GetPgJsonbNormalizedValue(
            placeholder_value));
    absl::StatusOr<googlesql::JSONValue> options =
        googlesql::JSONValue::ParseJSONString(std::string(jsonb));
    if (!options.ok()) {
      return error::ApproxDistanceFunctionInvalidJsonOption(
          node->function()->Name());
    }
    GOOGLESQL_RETURN_IF_ERROR(
        ValidateOptions(options->GetConstRef(), node->function()->Name()));
  }
  std::vector<std::unique_ptr<googlesql::ResolvedExpr>> argument_list;
  googlesql::FunctionArgumentTypeList argument_types;
  for (int i = 0; i < node->signature().arguments().size() - 1; ++i) {
    GOOGLESQL_ASSIGN_OR_RETURN(argument_list.emplace_back(),
                     Copy(node->argument_list(i)));
    argument_types.push_back(node->signature().argument(i));
  }
  googlesql::FunctionSignature new_signature(
      node->signature().result_type(), argument_types,
      node->signature().context_id(), node->signature().options());
  std::unique_ptr<googlesql::ResolvedFunctionCall> new_node =
      googlesql::MakeResolvedFunctionCall(
          node->type(), node->function(), new_signature,
          std::move(argument_list), node->error_mode());
  ann_functions_.insert(new_node.get());
  PushNodeToStack(std::move(new_node));
  return absl::OkStatus();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
