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

#include "backend/query/plan/query_plan_builder.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/spanner/v1/query_plan.pb.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_column.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::google::protobuf::Struct;
using ::googlesql::ResolvedExpr;
using ::googlesql::ResolvedScan;

constexpr v1::PlanNode::Kind kRelational = v1::PlanNode::RELATIONAL;
constexpr v1::PlanNode::Kind kScalar = v1::PlanNode::SCALAR;

// Returns how plan descriptions reference `column`, e.g. $SingerId.
std::string ColumnReference(const googlesql::ResolvedColumn& column) {
  return absl::StartsWith(column.name(), "$")
             ? column.name()
             : absl::StrCat("$", column.name());
}

void AddStat(Struct* stats, absl::string_view name, std::string total,
             absl::string_view unit) {
  Struct* stat =
      (*stats->mutable_fields())[std::string(name)].mutable_struct_value();
  (*stat->mutable_fields())["total"].set_string_value(std::move(total));
  (*stat->mutable_fields())["unit"].set_string_value(std::string(unit));
}

std::string Milliseconds(absl::Duration duration) {
  return absl::StrFormat("%.2f", absl::ToDoubleMilliseconds(duration));
}

// Returns execution statistics in the shape of Cloud Spanner's plan nodes.
// Clients such as spanner-cli parse every value as a string.
Struct ExecutionStats(int64_t rows, std::optional<int64_t> scanned_rows,
                      absl::Duration latency,
                      std::optional<absl::Duration> cpu_time,
                      int64_t executions) {
  Struct stats;
  AddStat(&stats, "rows", absl::StrCat(rows), "rows");
  if (scanned_rows.has_value()) {
    AddStat(&stats, "scanned_rows", absl::StrCat(*scanned_rows), "rows");
  }
  AddStat(&stats, "latency", Milliseconds(latency), "msecs");
  if (cpu_time.has_value()) {
    AddStat(&stats, "cpu_time", Milliseconds(*cpu_time), "msecs");
  }
  Struct* summary = (*stats.mutable_fields())["execution_summary"]
                        .mutable_struct_value();
  (*summary->mutable_fields())["num_executions"].set_string_value(
      absl::StrCat(executions));
  return stats;
}

// Describes a call of `function` with the described `arguments`, rendering
// operators the way Cloud Spanner plans do, e.g. ($SingerId = 1).
std::string FunctionDescription(const googlesql::Function& function,
                                const std::vector<std::string>& arguments,
                                bool distinct) {
  static const auto* kInfixOperators =
      new absl::flat_hash_map<std::string, std::string>({
          {"$equal", "="},
          {"$not_equal", "!="},
          {"$less", "<"},
          {"$less_or_equal", "<="},
          {"$greater", ">"},
          {"$greater_or_equal", ">="},
          {"$add", "+"},
          {"$subtract", "-"},
          {"$multiply", "*"},
          {"$divide", "/"},
          {"$and", "AND"},
          {"$or", "OR"},
          {"$concat_op", "||"},
          {"$like", "LIKE"},
          {"$is_distinct", "IS DISTINCT FROM"},
          {"$is_not_distinct", "IS NOT DISTINCT FROM"},
      });
  static const auto* kPostfixOperators =
      new absl::flat_hash_map<std::string, std::string>({
          {"$is_null", "IS NULL"},
          {"$is_true", "IS TRUE"},
          {"$is_false", "IS FALSE"},
      });
  const std::string& name = function.Name();
  if (auto it = kInfixOperators->find(name);
      it != kInfixOperators->end() && arguments.size() >= 2) {
    return absl::StrCat(
        "(", absl::StrJoin(arguments, absl::StrCat(" ", it->second, " ")),
        ")");
  }
  if (auto it = kPostfixOperators->find(name);
      it != kPostfixOperators->end() && arguments.size() == 1) {
    return absl::StrCat("(", arguments[0], " ", it->second, ")");
  }
  if (name == "$not" && arguments.size() == 1) {
    return absl::StrCat("(NOT ", arguments[0], ")");
  }
  if (name == "$unary_minus" && arguments.size() == 1) {
    return absl::StrCat("(-", arguments[0], ")");
  }
  if (name == "$in" && arguments.size() >= 2) {
    return absl::StrCat(
        "(", arguments[0], " IN (",
        absl::StrJoin(arguments.begin() + 1, arguments.end(), ", "), "))");
  }
  if (name == "$between" && arguments.size() == 3) {
    return absl::StrCat("(", arguments[0], " BETWEEN ", arguments[1], " AND ",
                        arguments[2], ")");
  }
  if (name == "$count_star") {
    return "COUNT(*)";
  }
  if (name == "$make_array") {
    return absl::StrCat("[", absl::StrJoin(arguments, ", "), "]");
  }
  absl::string_view sql_name = name;
  absl::ConsumePrefix(&sql_name, "$");
  return absl::StrCat(absl::AsciiStrToUpper(sql_name), "(",
                      distinct ? "DISTINCT " : "",
                      absl::StrJoin(arguments, ", "), ")");
}

std::string SubqueryDisplayName(
    googlesql::ResolvedSubqueryExpr::SubqueryType type) {
  switch (type) {
    case googlesql::ResolvedSubqueryExpr::SCALAR:
      return "Scalar Subquery";
    case googlesql::ResolvedSubqueryExpr::ARRAY:
      return "Array Subquery";
    case googlesql::ResolvedSubqueryExpr::EXISTS:
      return "Exists Subquery";
    case googlesql::ResolvedSubqueryExpr::IN:
      return "In Subquery";
    default:
      return "Subquery";
  }
}

std::string JoinTypeName(googlesql::ResolvedJoinScan::JoinType type) {
  switch (type) {
    case googlesql::ResolvedJoinScan::INNER:
      return "INNER";
    case googlesql::ResolvedJoinScan::LEFT:
      return "LEFT";
    case googlesql::ResolvedJoinScan::RIGHT:
      return "RIGHT";
    case googlesql::ResolvedJoinScan::FULL:
      return "FULL";
  }
}

// Returns the lower-case value of the string hint `name` of `node`, if any.
std::string StringHint(const googlesql::ResolvedScan* node,
                       absl::string_view name) {
  for (const auto& hint : node->hint_list()) {
    if (absl::EqualsIgnoreCase(hint->name(), name) &&
        hint->value()->Is<googlesql::ResolvedLiteral>()) {
      const googlesql::Value& value =
          hint->value()->GetAs<googlesql::ResolvedLiteral>()->value();
      if (!value.is_null() && value.type()->IsString()) {
        return absl::AsciiStrToLower(value.string_value());
      }
    }
  }
  return "";
}

// Returns `scan` without the projections that only pass columns through.
const ResolvedScan* SkipPassThrough(const ResolvedScan* scan) {
  while (scan->Is<googlesql::ResolvedProjectScan>() &&
         scan->GetAs<googlesql::ResolvedProjectScan>()->expr_list_size() ==
             0) {
    scan = scan->GetAs<googlesql::ResolvedProjectScan>()->input_scan();
  }
  return scan;
}

// Appends the conjuncts of `expr` to `conjuncts`.
void AddConjuncts(const ResolvedExpr* expr,
                  std::vector<const ResolvedExpr*>* conjuncts) {
  if (expr->Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expr->GetAs<googlesql::ResolvedFunctionCall>();
    if (call->function()->Name() == "$and") {
      for (const auto& argument : call->argument_list()) {
        AddConjuncts(argument.get(), conjuncts);
      }
      return;
    }
  }
  conjuncts->push_back(expr);
}

bool IsConstant(const ResolvedExpr* expr) {
  if (expr->Is<googlesql::ResolvedCast>()) {
    return IsConstant(expr->GetAs<googlesql::ResolvedCast>()->expr());
  }
  return expr->Is<googlesql::ResolvedLiteral>() ||
         expr->Is<googlesql::ResolvedParameter>();
}

// A condition that a table scan can use to seek to the rows it reads.
struct SeekCondition {
  // The index of the constrained table column.
  int column_index = -1;
  // The condition fixes the column to one or more values.
  bool equality = false;
};

// Returns the seek condition that `conjunct` expresses on the columns of
// `scan`, if any: a comparison of a column with constants.
std::optional<SeekCondition> AsSeekCondition(
    const ResolvedExpr* conjunct, const googlesql::ResolvedTableScan* scan) {
  if (!conjunct->Is<googlesql::ResolvedFunctionCall>()) {
    return std::nullopt;
  }
  const auto* call = conjunct->GetAs<googlesql::ResolvedFunctionCall>();
  static const auto* kSeekFunctions = new absl::flat_hash_set<std::string>({
      "$equal", "$in", "$less", "$less_or_equal", "$greater",
      "$greater_or_equal", "$between"});
  const std::string& name = call->function()->Name();
  if (!kSeekFunctions->contains(name) || call->argument_list_size() < 2) {
    return std::nullopt;
  }
  // Comparisons may have the column on either side; other functions take it
  // first.
  int column_argument = 0;
  if (name != "$in" && name != "$between" &&
      !call->argument_list(0)->Is<googlesql::ResolvedColumnRef>()) {
    column_argument = 1;
  }
  const ResolvedExpr* column_expr = call->argument_list(column_argument);
  if (!column_expr->Is<googlesql::ResolvedColumnRef>()) {
    return std::nullopt;
  }
  for (int i = 0; i < call->argument_list_size(); ++i) {
    if (i != column_argument && !IsConstant(call->argument_list(i))) {
      return std::nullopt;
    }
  }
  const googlesql::ResolvedColumn& column =
      column_expr->GetAs<googlesql::ResolvedColumnRef>()->column();
  for (int i = 0; i < scan->column_list_size(); ++i) {
    if (scan->column_list(i) == column) {
      return SeekCondition{.column_index = scan->column_index_list(i),
                           .equality = name == "$equal" || name == "$in"};
    }
  }
  return std::nullopt;
}

class PlanBuilder {
 public:
  explicit PlanBuilder(const StatementProfile* profile) : profile_(profile) {}

  v1::QueryPlan Build(const googlesql::ResolvedStatement& statement) {
    int root = -1;
    switch (statement.node_kind()) {
      case googlesql::RESOLVED_QUERY_STMT: {
        const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
        root = AddNode(kRelational, "Serialize Result");
        SetMetadata(root, "execution_method", "Row");
        Link(root, AddScan(query->query()));
        for (const auto& output_column : query->output_column_list()) {
          Link(root, AddScalar("Reference",
                               ColumnReference(output_column->column())));
        }
        break;
      }
      case googlesql::RESOLVED_INSERT_STMT: {
        const auto* insert = statement.GetAs<googlesql::ResolvedInsertStmt>();
        root = AddApplyMutations(InsertOperation(insert->insert_mode()),
                                 insert->table_scan()->table());
        Link(root, insert->query() != nullptr
                       ? AddScan(insert->query())
                       : AddNode(kRelational, "Unit Relation"));
        break;
      }
      case googlesql::RESOLVED_UPDATE_STMT: {
        const auto* update = statement.GetAs<googlesql::ResolvedUpdateStmt>();
        root = AddApplyMutations("UPDATE", update->table_scan()->table());
        Link(root, AddTableAccess(update->table_scan(), update->where_expr()));
        for (const auto& item : update->update_item_list()) {
          std::string target;
          if (item->target()->Is<googlesql::ResolvedColumnRef>()) {
            target = item->target()
                         ->GetAs<googlesql::ResolvedColumnRef>()
                         ->column()
                         .name();
          }
          if (item->set_value() != nullptr) {
            LinkExpr(root, item->set_value()->value(), "", target);
          }
        }
        break;
      }
      case googlesql::RESOLVED_DELETE_STMT: {
        const auto* del = statement.GetAs<googlesql::ResolvedDeleteStmt>();
        root = AddApplyMutations("DELETE", del->table_scan()->table());
        Link(root, AddTableAccess(del->table_scan(), del->where_expr()));
        break;
      }
      default:
        root = AddNode(kRelational, statement.node_kind_string());
        break;
    }
    if (profile_ != nullptr) {
      *node(root).mutable_execution_stats() =
          ExecutionStats(profile_->rows, std::nullopt, profile_->latency,
                         profile_->cpu_time, /*executions=*/1);
    }
    return std::move(plan_);
  }

 private:
  static std::string InsertOperation(
      googlesql::ResolvedInsertStmt::InsertMode mode) {
    switch (mode) {
      case googlesql::ResolvedInsertStmt::OR_IGNORE:
        return "INSERT_OR_IGNORE";
      case googlesql::ResolvedInsertStmt::OR_REPLACE:
        return "REPLACE";
      case googlesql::ResolvedInsertStmt::OR_UPDATE:
        return "INSERT_OR_UPDATE";
      default:
        return "INSERT";
    }
  }

  v1::PlanNode& node(int index) { return *plan_.mutable_plan_nodes(index); }

  // Appends a node and returns its index, which is also its position.
  int AddNode(v1::PlanNode::Kind kind, absl::string_view display_name) {
    const int index = plan_.plan_nodes_size();
    v1::PlanNode* plan_node = plan_.add_plan_nodes();
    plan_node->set_index(index);
    plan_node->set_kind(kind);
    plan_node->set_display_name(std::string(display_name));
    return index;
  }

  int AddScalar(absl::string_view display_name, std::string description) {
    const int index = AddNode(kScalar, display_name);
    node(index).mutable_short_representation()->set_description(
        std::move(description));
    return index;
  }

  void Link(int parent, int child, absl::string_view type = "",
            absl::string_view variable = "") {
    v1::PlanNode::ChildLink* link = node(parent).add_child_links();
    link->set_child_index(child);
    if (!type.empty()) {
      link->set_type(std::string(type));
    }
    if (!variable.empty()) {
      link->set_variable(std::string(variable));
    }
  }

  void SetMetadata(int index, absl::string_view key, absl::string_view value) {
    (*node(index).mutable_metadata()->mutable_fields())[std::string(key)]
        .set_string_value(std::string(value));
  }

  const std::string& Description(int index) {
    return node(index).short_representation().description();
  }

  int AddApplyMutations(absl::string_view operation,
                        const googlesql::Table* table) {
    const int index = AddNode(kRelational, "Apply Mutations");
    SetMetadata(index, "operation_type", operation);
    SetMetadata(index, "table", table->FullName());
    SetMetadata(index, "execution_method", "Row");
    return index;
  }

  // Adds the scalar tree of `expr` as a child of the relational node
  // `parent`. The subqueries of the expression are also linked to `parent`,
  // with the Scalar link type.
  int LinkExpr(int parent, const ResolvedExpr* expr, absl::string_view type,
               absl::string_view variable = "") {
    std::vector<int> outer_subqueries;
    outer_subqueries.swap(subqueries_);
    const int child = AddExpr(expr);
    Link(parent, child, type, variable);
    for (int subquery : subqueries_) {
      Link(parent, subquery, "Scalar");
    }
    subqueries_ = std::move(outer_subqueries);
    return child;
  }

  // Adds the scalar tree of `expr` and returns the index of its root.
  int AddExpr(const ResolvedExpr* expr) {
    const int index = AddNode(kScalar, "Function");
    std::vector<std::string> arguments;
    auto add_argument = [&](const ResolvedExpr* argument,
                            absl::string_view type = "") {
      const int child = AddExpr(argument);
      Link(index, child, type);
      arguments.push_back(Description(child));
      // Descriptions reference the subqueries of their arguments.
      for (const auto& [variable, subquery] :
           node(child).short_representation().subqueries()) {
        (*node(index).mutable_short_representation()->mutable_subqueries())
            [variable] = subquery;
      }
    };

    std::string description;
    switch (expr->node_kind()) {
      case googlesql::RESOLVED_COLUMN_REF:
        node(index).set_display_name("Reference");
        description = ColumnReference(
            expr->GetAs<googlesql::ResolvedColumnRef>()->column());
        break;
      case googlesql::RESOLVED_LITERAL: {
        node(index).set_display_name("Constant");
        const googlesql::Value& value =
            expr->GetAs<googlesql::ResolvedLiteral>()->value();
        description = value.type()->IsExtendedType()
                          ? value.DebugString()
                          : value.GetSQLLiteral(googlesql::PRODUCT_EXTERNAL);
        break;
      }
      case googlesql::RESOLVED_PARAMETER: {
        node(index).set_display_name("Parameter");
        const auto* parameter = expr->GetAs<googlesql::ResolvedParameter>();
        const std::string name = parameter->name().empty()
                                     ? absl::StrCat(parameter->position())
                                     : parameter->name();
        description = parameter->name().empty() ? absl::StrCat("$", name)
                                                : absl::StrCat("@", name);
        SetMetadata(index, "parameter_reference", name);
        SetMetadata(index, "parameter_type",
                    parameter->type()->IsArray() ? "array" : "scalar");
        break;
      }
      case googlesql::RESOLVED_FUNCTION_CALL:
      case googlesql::RESOLVED_AGGREGATE_FUNCTION_CALL:
      case googlesql::RESOLVED_ANALYTIC_FUNCTION_CALL: {
        const auto* call = expr->GetAs<googlesql::ResolvedFunctionCallBase>();
        for (const auto& argument : call->argument_list()) {
          add_argument(argument.get());
        }
        bool distinct = false;
        if (call->Is<googlesql::ResolvedNonScalarFunctionCallBase>()) {
          distinct =
              call->GetAs<googlesql::ResolvedNonScalarFunctionCallBase>()
                  ->distinct();
        }
        description =
            FunctionDescription(*call->function(), arguments, distinct);
        break;
      }
      case googlesql::RESOLVED_CAST: {
        add_argument(expr->GetAs<googlesql::ResolvedCast>()->expr());
        description =
            absl::StrCat("CAST(", arguments[0], " AS ",
                         expr->type()->TypeName(googlesql::PRODUCT_EXTERNAL),
                         ")");
        break;
      }
      case googlesql::RESOLVED_SUBQUERY_EXPR: {
        const auto* subquery = expr->GetAs<googlesql::ResolvedSubqueryExpr>();
        node(index).set_display_name(
            SubqueryDisplayName(subquery->subquery_type()));
        if (subquery->in_expr() != nullptr) {
          add_argument(subquery->in_expr(), "Value");
        }
        const std::string variable =
            absl::StrCat("sq_", ++subquery_count_);
        Link(index, AddScan(subquery->subquery()));
        (*node(index).mutable_short_representation()->mutable_subqueries())
            [variable] = index;
        const std::string reference = absl::StrCat("$", variable);
        switch (subquery->subquery_type()) {
          case googlesql::ResolvedSubqueryExpr::EXISTS:
            description = absl::StrCat("EXISTS(", reference, ")");
            break;
          case googlesql::ResolvedSubqueryExpr::IN:
            description =
                absl::StrCat("(", arguments[0], " IN ", reference, ")");
            break;
          default:
            description = reference;
            break;
        }
        subqueries_.push_back(index);
        break;
      }
      default: {
        std::vector<const googlesql::ResolvedNode*> children;
        expr->GetChildNodes(&children);
        for (const googlesql::ResolvedNode* child : children) {
          if (child->IsExpression()) {
            add_argument(child->GetAs<ResolvedExpr>());
          }
        }
        description = absl::StrCat(absl::AsciiStrToUpper(
                                       expr->node_kind_string()),
                                   "(", absl::StrJoin(arguments, ", "), ")");
        break;
      }
    }
    node(index).mutable_short_representation()->set_description(description);
    return index;
  }

  // Adds the relational tree of `scan` and returns the index of its root.
  int AddScan(const ResolvedScan* scan) {
    switch (scan->node_kind()) {
      case googlesql::RESOLVED_TABLE_SCAN:
        return AddTableAccess(scan->GetAs<googlesql::ResolvedTableScan>(),
                              /*filter=*/nullptr);
      case googlesql::RESOLVED_FILTER_SCAN: {
        const auto* filter = scan->GetAs<googlesql::ResolvedFilterScan>();
        const ResolvedScan* input = SkipPassThrough(filter->input_scan());
        if (input->Is<googlesql::ResolvedTableScan>()) {
          return AddTableAccess(input->GetAs<googlesql::ResolvedTableScan>(),
                                filter->filter_expr());
        }
        const int index = AddNode(kRelational, "Filter");
        Link(index, AddScan(filter->input_scan()));
        LinkExpr(index, filter->filter_expr(), "Condition");
        return index;
      }
      case googlesql::RESOLVED_PROJECT_SCAN: {
        const auto* project = scan->GetAs<googlesql::ResolvedProjectScan>();
        if (project->expr_list_size() == 0) {
          return AddScan(project->input_scan());
        }
        const int index = AddNode(kRelational, "Compute");
        Link(index, AddScan(project->input_scan()));
        for (const auto& computed : project->expr_list()) {
          LinkExpr(index, computed->expr(), "", computed->column().name());
        }
        return index;
      }
      case googlesql::RESOLVED_JOIN_SCAN:
        return AddJoin(scan->GetAs<googlesql::ResolvedJoinScan>());
      case googlesql::RESOLVED_ARRAY_SCAN: {
        const auto* array = scan->GetAs<googlesql::ResolvedArrayScan>();
        if (array->input_scan() == nullptr) {
          return AddArrayUnnest(array);
        }
        const int index = AddNode(
            kRelational, array->is_outer() ? "Outer Apply" : "Cross Apply");
        Link(index, AddScan(array->input_scan()), "Input");
        Link(index, AddArrayUnnest(array), "Map");
        if (array->join_expr() != nullptr) {
          LinkExpr(index, array->join_expr(), "Condition");
        }
        return index;
      }
      case googlesql::RESOLVED_AGGREGATE_SCAN: {
        const auto* aggregate = scan->GetAs<googlesql::ResolvedAggregateScan>();
        const int index = AddNode(kRelational, "Aggregate");
        SetMetadata(index, "call_type", "Global");
        SetMetadata(index, "iterator_type",
                    aggregate->group_by_list_size() == 0 ? "Stream" : "Hash");
        Link(index, AddScan(aggregate->input_scan()));
        for (const auto& key : aggregate->group_by_list()) {
          LinkExpr(index, key->expr(), "Key", key->column().name());
        }
        for (const auto& aggregation : aggregate->aggregate_list()) {
          LinkExpr(index, aggregation->expr(), "Agg",
                   aggregation->column().name());
        }
        return index;
      }
      case googlesql::RESOLVED_ORDER_BY_SCAN: {
        const auto* order_by = scan->GetAs<googlesql::ResolvedOrderByScan>();
        const int index = AddNode(kRelational, "Sort");
        Link(index, AddScan(order_by->input_scan()));
        AddSortKeys(index, order_by);
        return index;
      }
      case googlesql::RESOLVED_LIMIT_OFFSET_SCAN:
        return AddLimit(scan->GetAs<googlesql::ResolvedLimitOffsetScan>());
      case googlesql::RESOLVED_SET_OPERATION_SCAN:
        return AddSetOperation(
            scan->GetAs<googlesql::ResolvedSetOperationScan>());
      case googlesql::RESOLVED_SINGLE_ROW_SCAN:
        return AddNode(kRelational, "Unit Relation");
      case googlesql::RESOLVED_WITH_SCAN: {
        const auto* with = scan->GetAs<googlesql::ResolvedWithScan>();
        for (const auto& entry : with->with_entry_list()) {
          with_queries_[entry->with_query_name()] = entry->with_subquery();
        }
        return AddScan(with->query());
      }
      case googlesql::RESOLVED_WITH_REF_SCAN: {
        // Shows the WITH query at each reference, which keeps the plan a
        // tree. References inside a recursive WITH query are not expanded.
        const std::string& name =
            scan->GetAs<googlesql::ResolvedWithRefScan>()->with_query_name();
        auto it = with_queries_.find(name);
        if (it == with_queries_.end() || !expanding_.insert(name).second) {
          return AddGenericScan(scan);
        }
        const int index = AddScan(it->second);
        expanding_.erase(name);
        return index;
      }
      case googlesql::RESOLVED_ANALYTIC_SCAN: {
        const auto* analytic = scan->GetAs<googlesql::ResolvedAnalyticScan>();
        const int index = AddNode(kRelational, "Analytic Evaluator");
        Link(index, AddScan(analytic->input_scan()));
        for (const auto& group : analytic->function_group_list()) {
          for (const auto& function : group->analytic_function_list()) {
            LinkExpr(index, function->expr(), "", function->column().name());
          }
        }
        return index;
      }
      case googlesql::RESOLVED_TVFSCAN: {
        const auto* tvf = scan->GetAs<googlesql::ResolvedTVFScan>();
        const int index = AddNode(kRelational, "TVF");
        SetMetadata(index, "function", tvf->tvf()->FullName());
        for (const auto& argument : tvf->argument_list()) {
          if (argument->scan() != nullptr) {
            Link(index, AddScan(argument->scan()), "Input");
          }
        }
        return index;
      }
      case googlesql::RESOLVED_SAMPLE_SCAN: {
        const auto* sample = scan->GetAs<googlesql::ResolvedSampleScan>();
        const int index = AddNode(kRelational, "Sample");
        Link(index, AddScan(sample->input_scan()));
        return index;
      }
      default:
        return AddGenericScan(scan);
    }
  }

  // Adds a node named after the kind of `scan`, with its input scans.
  int AddGenericScan(const ResolvedScan* scan) {
    const int index = AddNode(kRelational, scan->node_kind_string());
    std::vector<const googlesql::ResolvedNode*> children;
    scan->GetChildNodes(&children);
    for (const googlesql::ResolvedNode* child : children) {
      if (child->IsScan()) {
        Link(index, AddScan(child->GetAs<ResolvedScan>()));
      }
    }
    return index;
  }

  // Adds a table access: a Distributed Union over a Scan of the table, with a
  // Filter Scan in between if `filter` is set. Conjuncts of `filter` that fix
  // a prefix of the primary key become seek conditions (and the split range
  // of the union); the others are residual conditions.
  int AddTableAccess(const googlesql::ResolvedTableScan* scan,
                     const ResolvedExpr* filter) {
    const googlesql::Table* table = scan->table();
    const int union_index = AddNode(kRelational, "Distributed Union");
    SetMetadata(union_index, "distribution_table", table->FullName());
    SetMetadata(union_index, "execution_method", "Row");
    SetMetadata(union_index, "split_ranges_aligned", "false");

    std::vector<const ResolvedExpr*> conjuncts;
    if (filter != nullptr) {
      AddConjuncts(filter, &conjuncts);
    }
    std::vector<std::optional<SeekCondition>> seek_conditions;
    for (const ResolvedExpr* conjunct : conjuncts) {
      seek_conditions.push_back(AsSeekCondition(conjunct, scan));
    }
    // The primary key columns that seek conditions constrain, as a prefix:
    // equalities extend the prefix and a range ends it.
    absl::flat_hash_set<int> seek_columns;
    for (int key_column : table->PrimaryKey().value_or(std::vector<int>())) {
      bool constrained = false;
      bool equality = false;
      for (const auto& condition : seek_conditions) {
        if (condition.has_value() && condition->column_index == key_column) {
          constrained = true;
          equality |= condition->equality;
        }
      }
      if (!constrained) {
        break;
      }
      seek_columns.insert(key_column);
      if (!equality) {
        break;
      }
    }
    std::vector<const ResolvedExpr*> seeks;
    std::vector<const ResolvedExpr*> residuals;
    for (int i = 0; i < conjuncts.size(); ++i) {
      const bool seek = seek_conditions[i].has_value() &&
                        seek_columns.contains(seek_conditions[i]->column_index);
      (seek ? seeks : residuals).push_back(conjuncts[i]);
    }

    int child = -1;
    if (filter == nullptr) {
      child = AddTableScanNode(scan, /*full_scan=*/true);
    } else {
      child = AddNode(kRelational, "Filter Scan");
      if (!seek_columns.empty()) {
        SetMetadata(child, "seekable_key_size",
                    absl::StrCat(seek_columns.size()));
      }
      Link(child, AddTableScanNode(scan, /*full_scan=*/seeks.empty()));
      for (const ResolvedExpr* seek : seeks) {
        LinkExpr(child, seek, "Seek Condition");
      }
      for (const ResolvedExpr* residual : residuals) {
        LinkExpr(child, residual, "Residual Condition");
      }
    }
    Link(union_index, child);
    SetMetadata(union_index, "subquery_cluster_node", absl::StrCat(child));
    for (const ResolvedExpr* seek : seeks) {
      LinkExpr(union_index, seek, "Split Range");
    }
    return union_index;
  }

  int AddTableScanNode(const googlesql::ResolvedTableScan* scan,
                       bool full_scan) {
    const googlesql::Table* table = scan->table();
    const int index = AddNode(kRelational, "Scan");
    const std::string force_index = StringHint(scan, "force_index");
    if (!force_index.empty() && force_index != "_base_table") {
      SetMetadata(index, "scan_type", "IndexScan");
      SetMetadata(index, "scan_target", force_index);
    } else {
      SetMetadata(index, "scan_type", "TableScan");
      SetMetadata(index, "scan_target", table->FullName());
    }
    SetMetadata(index, "execution_method", "Row");
    if (full_scan) {
      SetMetadata(index, "Full scan", "true");
    }
    for (int column_index : scan->column_index_list()) {
      const std::string& name = table->GetColumn(column_index)->Name();
      Link(index, AddScalar("Reference", name), "", name);
    }
    if (profile_ != nullptr) {
      auto it = profile_->scans.find(scan);
      if (it != profile_->scans.end()) {
        const ScanProfile& scan_profile = it->second;
        *node(index).mutable_execution_stats() = ExecutionStats(
            scan_profile.rows, scan_profile.rows, scan_profile.latency,
            std::nullopt, scan_profile.executions);
      }
    }
    return index;
  }

  int AddJoin(const googlesql::ResolvedJoinScan* join) {
    const googlesql::ResolvedJoinScan::JoinType type = join->join_type();
    const std::string method = StringHint(join, "join_method");
    if (type == googlesql::ResolvedJoinScan::FULL || method == "hash_join" ||
        method == "merge_join" || method == "push_broadcast_hash_join") {
      const int index = AddNode(
          kRelational, method == "merge_join" ? "Merge Join"
                       : method == "push_broadcast_hash_join"
                           ? "Push Broadcast Hash Join"
                           : "Hash Join");
      SetMetadata(index, "join_type", JoinTypeName(type));
      Link(index, AddScan(join->left_scan()), "Build");
      Link(index, AddScan(join->right_scan()), "Probe");
      if (join->join_expr() != nullptr) {
        LinkExpr(index, join->join_expr(), "Condition");
      }
      return index;
    }
    // Apply joins evaluate the map side for each row of the input side. A
    // right join preserves the rows of its right side, which becomes the
    // input.
    const bool swap = type == googlesql::ResolvedJoinScan::RIGHT;
    const int index =
        AddNode(kRelational, type == googlesql::ResolvedJoinScan::INNER
                                 ? "Cross Apply"
                                 : "Outer Apply");
    Link(index, AddScan(swap ? join->right_scan() : join->left_scan()),
         "Input");
    const ResolvedScan* map = swap ? join->left_scan() : join->right_scan();
    if (join->join_expr() == nullptr) {
      Link(index, AddScan(map), "Map");
      return index;
    }
    const int filter = AddNode(kRelational, "Filter");
    Link(index, filter, "Map");
    Link(filter, AddScan(map));
    LinkExpr(filter, join->join_expr(), "Condition");
    return index;
  }

  int AddArrayUnnest(const googlesql::ResolvedArrayScan* array) {
    const int index = AddNode(kRelational, "Array Unnest");
    for (const auto& array_expr : array->array_expr_list()) {
      LinkExpr(index, array_expr.get(), "");
    }
    return index;
  }

  void AddSortKeys(int index, const googlesql::ResolvedOrderByScan* order_by) {
    for (const auto& item : order_by->order_by_item_list()) {
      const int key = LinkExpr(index, item->column_ref(), "Key");
      node(key).mutable_short_representation()->set_description(
          absl::StrCat(Description(key),
                       item->is_descending() ? " DESC" : " ASC"));
    }
  }

  int AddLimit(const googlesql::ResolvedLimitOffsetScan* limit) {
    const ResolvedScan* input = SkipPassThrough(limit->input_scan());
    int index = -1;
    if (input->Is<googlesql::ResolvedOrderByScan>()) {
      const auto* order_by = input->GetAs<googlesql::ResolvedOrderByScan>();
      index = AddNode(kRelational, "Sort Limit");
      SetMetadata(index, "call_type", "Global");
      Link(index, AddScan(order_by->input_scan()));
      AddSortKeys(index, order_by);
    } else {
      index = AddNode(kRelational, "Limit");
      SetMetadata(index, "call_type", "Global");
      Link(index, AddScan(limit->input_scan()));
    }
    if (limit->limit() != nullptr) {
      LinkExpr(index, limit->limit(), "Limit");
    }
    if (limit->offset() != nullptr) {
      LinkExpr(index, limit->offset(), "Offset");
    }
    return index;
  }

  int AddSetOperation(const googlesql::ResolvedSetOperationScan* set) {
    const auto op = set->op_type();
    if (op != googlesql::ResolvedSetOperationScan::UNION_ALL &&
        op != googlesql::ResolvedSetOperationScan::UNION_DISTINCT) {
      const bool intersect =
          op == googlesql::ResolvedSetOperationScan::INTERSECT_ALL ||
          op == googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT;
      const int index =
          AddNode(kRelational, intersect ? "Semi Apply" : "Anti Semi Apply");
      for (int i = 0; i < set->input_item_list_size(); ++i) {
        Link(index, AddScan(set->input_item_list(i)->scan()),
             i == 0 ? "Input" : "Map");
      }
      return index;
    }
    int index = -1;
    int union_all = -1;
    if (op == googlesql::ResolvedSetOperationScan::UNION_DISTINCT) {
      // UNION DISTINCT removes duplicates with an aggregation.
      index = AddNode(kRelational, "Aggregate");
      SetMetadata(index, "call_type", "Global");
      SetMetadata(index, "iterator_type", "Hash");
      union_all = AddNode(kRelational, "Union All");
      Link(index, union_all);
    } else {
      index = union_all = AddNode(kRelational, "Union All");
    }
    for (const auto& item : set->input_item_list()) {
      const int input = AddNode(kRelational, "Union Input");
      Link(union_all, input);
      Link(input, AddScan(item->scan()));
      for (const googlesql::ResolvedColumn& column :
           item->output_column_list()) {
        Link(input, AddScalar("Reference", ColumnReference(column)));
      }
    }
    return index;
  }

  const StatementProfile* profile_;
  v1::QueryPlan plan_;
  // Subquery nodes to link to the relational node whose expression is being
  // added.
  std::vector<int> subqueries_;
  int subquery_count_ = 0;
  // The WITH queries in scope, by name, and those being expanded.
  absl::flat_hash_map<std::string, const ResolvedScan*> with_queries_;
  absl::flat_hash_set<std::string> expanding_;
};

}  // namespace

v1::QueryPlan BuildQueryPlan(const googlesql::ResolvedStatement& statement,
                             const StatementProfile* profile) {
  return PlanBuilder(profile).Build(statement);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
