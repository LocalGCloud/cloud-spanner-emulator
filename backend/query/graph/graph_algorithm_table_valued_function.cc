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

#include "backend/query/graph/graph_algorithm_table_valued_function.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/parser/ast_node_kind.h"
#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/graph_element_type.h"
#include "googlesql/public/types/graph_path_type.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/common/case.h"
#include "backend/query/analyzer_options.h"
#include "backend/query/graph/graph_algorithms.h"
#include "googlesql/base/status_macros.h"

namespace google::spanner::emulator::backend {
namespace {

using googlesql::FunctionArgumentType;
using googlesql::FunctionArgumentTypeOptions;
using googlesql::FunctionSignature;
using googlesql::GraphElementType;
using googlesql::TVFSchemaColumn;
using googlesql::Value;

enum class Algorithm {
  kPageRank,
  kBetweennessCentrality,
  kClosenessCentrality,
  kWeaklyConnectedComponents,
  kModularityClustering,
  kCorrelationClustering,
  kLabelPropagation,
  kCliqueFinding,
  kJaccardSimilarity,
  kCosineSimilarity,
  kCommonNeighborsSimilarity,
  kTotalNeighborsSimilarity,
  kShortestPath,
};

// The shape of an algorithm's output.
enum class Output {
  kScore,       // node, score
  kCentrality,  // node, centrality
  kCluster,     // node, cluster
  kClique,      // node, clique
  kSimilarity,  // source_node, target_node, similarity
  kPath,        // source_node, target_node, path, cost
};

enum class ParameterType {
  kInt64,
  kDouble,
  kString,
  kBool,
  kStringArray,
  // An array of graph nodes, such as `ARRAY {MATCH (n) RETURN n}`.
  kNodeArray,
};

struct Parameter {
  absl::string_view name;
  ParameterType type;
  bool required = false;
};

struct AlgorithmSpec {
  Algorithm algorithm;
  absl::string_view name;
  Output output;
  // Required parameters come first; an optional node array comes last, so it
  // is dropped rather than filled in with an untyped NULL when omitted.
  std::vector<Parameter> parameters;
};

constexpr absl::string_view kNodeLabels = "node_labels";
constexpr absl::string_view kEdgeLabels = "edge_labels";
constexpr absl::string_view kEdgeWeightProperty = "edge_weight_property";
constexpr absl::string_view kMachineCategory = "machine_category";
constexpr absl::string_view kZone = "zone";
constexpr absl::string_view kMaxIdleTime = "max_idle_time";
constexpr absl::string_view kSourceNodes = "source_nodes";
constexpr absl::string_view kTargetNodes = "target_nodes";
constexpr absl::string_view kDampingFactor = "damping_factor";
constexpr absl::string_view kMaxIterations = "max_iterations";
constexpr absl::string_view kApproxPrecision = "approx_precision";
constexpr absl::string_view kNumSourceNodes = "num_source_nodes";
constexpr absl::string_view kMode = "mode";
constexpr absl::string_view kUseWassermanFaust = "use_wasserman_faust";
constexpr absl::string_view kEpsilon = "epsilon";
constexpr absl::string_view kSampleSize = "sample_size";
constexpr absl::string_view kResolution = "resolution";
constexpr absl::string_view kMaxInnerIterations = "max_inner_iterations";
constexpr absl::string_view kSeedLabelProperty = "seed_label_property";
constexpr absl::string_view kMinDensity = "min_density";

// Parameters that every algorithm accepts, from the "Common algorithm input
// parameters" table of the Spanner Graph documentation.
constexpr Parameter kCommonParameters[] = {
    {kNodeLabels, ParameterType::kStringArray},
    {kEdgeLabels, ParameterType::kStringArray},
    {kEdgeWeightProperty, ParameterType::kString},
    {kMachineCategory, ParameterType::kString},
    {kZone, ParameterType::kString},
    {kMaxIdleTime, ParameterType::kString},
};

std::vector<AlgorithmSpec> AlgorithmSpecs() {
  const std::vector<Parameter> clustering = {
      {kMaxIterations, ParameterType::kInt64},
      {kMaxInnerIterations, ParameterType::kInt64},
  };
  const std::vector<Parameter> node_pairs = {
      {kSourceNodes, ParameterType::kNodeArray, /*required=*/true},
      {kTargetNodes, ParameterType::kNodeArray, /*required=*/true},
  };
  std::vector<Parameter> modularity = {{kResolution, ParameterType::kDouble}};
  modularity.insert(modularity.end(), clustering.begin(), clustering.end());
  std::vector<Parameter> correlation = {
      {kResolution, ParameterType::kDouble, /*required=*/true}};
  correlation.insert(correlation.end(), clustering.begin(), clustering.end());
  return {
      {Algorithm::kPageRank,
       "PageRank",
       Output::kScore,
       {{kDampingFactor, ParameterType::kDouble},
        {kMaxIterations, ParameterType::kInt64},
        {kApproxPrecision, ParameterType::kDouble},
        {kSourceNodes, ParameterType::kNodeArray}}},
      {Algorithm::kBetweennessCentrality,
       "BetweennessCentrality",
       Output::kCentrality,
       {{kNumSourceNodes, ParameterType::kInt64}}},
      {Algorithm::kClosenessCentrality,
       "ClosenessCentrality",
       Output::kCentrality,
       {{kMode, ParameterType::kString},
        {kUseWassermanFaust, ParameterType::kBool},
        {kEpsilon, ParameterType::kDouble},
        {kSampleSize, ParameterType::kInt64}}},
      {Algorithm::kWeaklyConnectedComponents,
       "WeaklyConnectedComponents",
       Output::kCluster,
       {}},
      {Algorithm::kModularityClustering, "ModularityClustering",
       Output::kCluster, modularity},
      {Algorithm::kCorrelationClustering, "CorrelationClustering",
       Output::kCluster, correlation},
      {Algorithm::kLabelPropagation,
       "LabelPropagation",
       Output::kCluster,
       {{kSeedLabelProperty, ParameterType::kString},
        {kMaxIterations, ParameterType::kInt64}}},
      {Algorithm::kCliqueFinding,
       "CliqueFinding",
       Output::kClique,
       {{kMinDensity, ParameterType::kDouble}}},
      {Algorithm::kJaccardSimilarity, "JaccardSimilarity",
       Output::kSimilarity, node_pairs},
      {Algorithm::kCosineSimilarity, "CosineSimilarity", Output::kSimilarity,
       node_pairs},
      {Algorithm::kCommonNeighborsSimilarity, "CommonNeighborsSimilarity",
       Output::kSimilarity, node_pairs},
      {Algorithm::kTotalNeighborsSimilarity, "TotalNeighborsSimilarity",
       Output::kSimilarity, node_pairs},
      {Algorithm::kShortestPath, "ShortestPath", Output::kPath, node_pairs},
  };
}

// Builds the two signatures of an algorithm: one takes the graph of the
// enclosing GQL query (`CALL Algo(...)`), the other the working table
// (`CALL PER () Algo(...)`). Both take the parameters as named arguments.
std::vector<FunctionSignature> MakeSignatures(const AlgorithmSpec& spec) {
  std::vector<Parameter> parameters;
  for (const Parameter& parameter : spec.parameters) {
    if (parameter.required) parameters.push_back(parameter);
  }
  parameters.insert(parameters.end(), std::begin(kCommonParameters),
                    std::end(kCommonParameters));
  for (const Parameter& parameter : spec.parameters) {
    if (!parameter.required) parameters.push_back(parameter);
  }

  std::vector<FunctionArgumentType> arguments;
  int num_node_arrays = 0;
  for (const Parameter& parameter : parameters) {
    FunctionArgumentTypeOptions options;
    options.set_argument_name(parameter.name, googlesql::kNamedOnly);
    if (!parameter.required) {
      options.set_cardinality(FunctionArgumentType::OPTIONAL);
    }
    switch (parameter.type) {
      case ParameterType::kInt64:
        arguments.emplace_back(googlesql::types::Int64Type(), options);
        break;
      case ParameterType::kDouble:
        arguments.emplace_back(googlesql::types::DoubleType(), options);
        break;
      case ParameterType::kString:
        arguments.emplace_back(googlesql::types::StringType(), options);
        break;
      case ParameterType::kBool:
        arguments.emplace_back(googlesql::types::BoolType(), options);
        break;
      case ParameterType::kStringArray:
        arguments.emplace_back(googlesql::types::StringArrayType(), options);
        break;
      case ParameterType::kNodeArray:
        // Source and target nodes may come from different element tables, so
        // they get independent templates.
        arguments.emplace_back(num_node_arrays++ == 0
                                   ? googlesql::ARG_KIND_EXPR_ARRAY_ANY_1
                                   : googlesql::ARG_KIND_EXPR_ARRAY_ANY_2,
                               options);
        break;
    }
  }

  std::vector<FunctionSignature> signatures;
  for (const FunctionArgumentType& input :
       {FunctionArgumentType::AnyGraph(),
        FunctionArgumentType::AnyRelation()}) {
    std::vector<FunctionArgumentType> signature_arguments = {input};
    signature_arguments.insert(signature_arguments.end(), arguments.begin(),
                               arguments.end());
    signatures.emplace_back(FunctionArgumentType::AnyRelation(),
                            std::move(signature_arguments),
                            /*context_ptr=*/nullptr);
  }
  return signatures;
}

// Expressions that collect every node or edge of a graph registered as `g`.
// They are expressions rather than queries because a statement cannot return
// graph elements.
constexpr absl::string_view kGraphAlias = "g";
constexpr absl::string_view kNodesExpression =
    "ARRAY(SELECT n FROM GRAPH_TABLE(g MATCH (n) RETURN n))";
constexpr absl::string_view kEdgesExpression =
    "ARRAY(SELECT e FROM GRAPH_TABLE(g MATCH -[e]-> RETURN e))";

absl::StatusOr<googlesql::AnalyzerOptions> ElementExpressionAnalyzerOptions() {
  googlesql::AnalyzerOptions options = MakeGoogleSqlAnalyzerOptions();
  options.mutable_language()->EnableLanguageFeature(
      googlesql::FEATURE_SQL_GRAPH_EXPOSE_GRAPH_ELEMENT);
  GOOGLESQL_RETURN_IF_ERROR(
      options.mutable_language()->EnableReservableKeyword("GRAPH_TABLE"));
  return options;
}

absl::StatusOr<bool> HasEdgeTables(const googlesql::PropertyGraph* graph) {
  absl::flat_hash_set<const googlesql::GraphEdgeTable*> edge_tables;
  GOOGLESQL_RETURN_IF_ERROR(graph->GetEdgeTables(edge_tables));
  return !edge_tables.empty();
}

// Returns the type of the elements that `expression` collects.
absl::StatusOr<const GraphElementType*> ElementType(
    const googlesql::PropertyGraph* graph, absl::string_view expression,
    googlesql::TypeFactory* type_factory) {
  googlesql::SimpleCatalog catalog("graph_algorithm");
  catalog.AddPropertyGraph(kGraphAlias, graph);
  GOOGLESQL_ASSIGN_OR_RETURN(googlesql::AnalyzerOptions options,
                   ElementExpressionAnalyzerOptions());
  std::unique_ptr<const googlesql::AnalyzerOutput> output;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeExpression(
      expression, options, &catalog, type_factory, &output));
  return output->resolved_expr()
      ->type()
      ->AsArray()
      ->element_type()
      ->AsGraphElement();
}

// Evaluates `expression` and returns the elements it collects.
absl::StatusOr<std::vector<Value>> ReadElements(
    const googlesql::PropertyGraph* graph, absl::string_view expression,
    googlesql::TypeFactory* type_factory) {
  googlesql::SimpleCatalog catalog("graph_algorithm");
  catalog.AddPropertyGraph(kGraphAlias, graph);
  googlesql::EvaluatorOptions evaluator_options;
  evaluator_options.type_factory = type_factory;
  googlesql::PreparedExpression prepared_expression(expression,
                                                    evaluator_options);
  GOOGLESQL_ASSIGN_OR_RETURN(googlesql::AnalyzerOptions options,
                   ElementExpressionAnalyzerOptions());
  GOOGLESQL_RETURN_IF_ERROR(prepared_expression.Prepare(options, &catalog));
  GOOGLESQL_ASSIGN_OR_RETURN(Value elements, prepared_expression.Execute());
  return elements.elements();
}

// The graph an algorithm runs on. `nodes` and `edges` are sorted by element
// table and key, and every edge connects two of the nodes.
struct InputGraph {
  std::vector<Value> nodes;
  std::vector<Value> edges;
  absl::flat_hash_map<std::string, int> node_index;
};

bool KeysLess(absl::Span<const Value> a, absl::Span<const Value> b) {
  return std::lexicographical_compare(
      a.begin(), a.end(), b.begin(), b.end(),
      [](const Value& x, const Value& y) { return x.LessThan(y); });
}

bool NodeLess(const Value& a, const Value& b) {
  if (a.GetDefinitionName() != b.GetDefinitionName()) {
    return a.GetDefinitionName() < b.GetDefinitionName();
  }
  return KeysLess(a.GetGraphNodeKeys(), b.GetGraphNodeKeys());
}

bool EdgeLess(const Value& a, const Value& b) {
  if (a.GetDefinitionName() != b.GetDefinitionName()) {
    return a.GetDefinitionName() < b.GetDefinitionName();
  }
  if (KeysLess(a.GetGraphEdgeSourceKeys(), b.GetGraphEdgeSourceKeys())) {
    return true;
  }
  if (KeysLess(b.GetGraphEdgeSourceKeys(), a.GetGraphEdgeSourceKeys())) {
    return false;
  }
  if (KeysLess(a.GetGraphEdgeDestKeys(), b.GetGraphEdgeDestKeys())) {
    return true;
  }
  if (KeysLess(b.GetGraphEdgeDestKeys(), a.GetGraphEdgeDestKeys())) {
    return false;
  }
  return a.GetIdentifier() < b.GetIdentifier();
}

bool HasAnyLabel(const Value& element, const Value& labels) {
  for (const std::string& label : element.GetLabels()) {
    for (const Value& wanted : labels.elements()) {
      if (!wanted.is_null() &&
          absl::EqualsIgnoreCase(label, wanted.string_value())) {
        return true;
      }
    }
  }
  return false;
}

// Builds the input graph from candidate nodes and edges, dropping duplicates
// and edges whose end nodes are not candidates.
InputGraph MakeInputGraph(std::vector<Value> nodes, std::vector<Value> edges) {
  InputGraph graph;
  std::sort(nodes.begin(), nodes.end(), NodeLess);
  for (Value& node : nodes) {
    if (graph.node_index
            .try_emplace(node.GetIdentifier(), graph.nodes.size())
            .second) {
      graph.nodes.push_back(std::move(node));
    }
  }
  std::sort(edges.begin(), edges.end(), EdgeLess);
  absl::flat_hash_set<std::string> edge_identifiers;
  for (Value& edge : edges) {
    if (graph.node_index.contains(edge.GetSourceNodeIdentifier()) &&
        graph.node_index.contains(edge.GetDestNodeIdentifier()) &&
        edge_identifiers.insert(std::string(edge.GetIdentifier())).second) {
      graph.edges.push_back(std::move(edge));
    }
  }
  return graph;
}

// The node and edge types of a relation's graph element columns.
absl::Status RelationElementTypes(const googlesql::TVFRelation& relation,
                                  absl::string_view algorithm,
                                  const GraphElementType*& node_type,
                                  const GraphElementType*& edge_type) {
  for (const TVFSchemaColumn& column : relation.columns()) {
    if (!column.type->IsGraphElement()) continue;
    const GraphElementType* type = column.type->AsGraphElement();
    const GraphElementType*& kind_type = type->IsNode() ? node_type : edge_type;
    if (kind_type != nullptr && !kind_type->Equals(type)) {
      return absl::InvalidArgumentError(absl::StrCat(
          algorithm, ": all ", type->IsNode() ? "node" : "edge",
          " columns of the CALL PER () input must have the same type"));
    }
    kind_type = type;
  }
  if (node_type == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        algorithm, ": the CALL PER () input must have a node column"));
  }
  return absl::OkStatus();
}

absl::StatusOr<InputGraph> ReadRelation(
    googlesql::EvaluatorTableIterator& relation) {
  std::vector<Value> nodes;
  std::vector<Value> edges;
  while (relation.NextRow()) {
    for (int i = 0; i < relation.NumColumns(); ++i) {
      const Value& value = relation.GetValue(i);
      if (!value.type()->IsGraphElement() || value.is_null()) continue;
      (value.IsNode() ? nodes : edges).push_back(value);
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(relation.Status());
  return MakeInputGraph(std::move(nodes), std::move(edges));
}

// The named arguments of a call, without the NULL ones.
class Arguments {
 public:
  Arguments(absl::string_view algorithm,
            absl::flat_hash_map<std::string, Value> values)
      : algorithm_(algorithm), values_(std::move(values)) {}

  const Value* Find(absl::string_view name) const {
    auto it = values_.find(name);
    return it == values_.end() ? nullptr : &it->second;
  }
  double Double(absl::string_view name, double default_value) const {
    const Value* value = Find(name);
    return value == nullptr ? default_value : value->double_value();
  }
  int64_t Int64(absl::string_view name, int64_t default_value) const {
    const Value* value = Find(name);
    return value == nullptr ? default_value : value->int64_value();
  }
  bool Bool(absl::string_view name, bool default_value) const {
    const Value* value = Find(name);
    return value == nullptr ? default_value : value->bool_value();
  }
  std::string String(absl::string_view name,
                     absl::string_view default_value) const {
    const Value* value = Find(name);
    return std::string(value == nullptr ? default_value
                                        : value->string_value());
  }

  absl::Status Error(absl::string_view name, absl::string_view message) const {
    return absl::InvalidArgumentError(
        absl::StrCat(algorithm_, ": ", name, " ", message));
  }

  // Checks the documented value constraints of the arguments that are set.
  absl::Status Validate() const {
    auto check = [&](absl::string_view name, auto valid,
                     absl::string_view message) -> absl::Status {
      const Value* value = Find(name);
      if (value != nullptr && !valid(*value)) return Error(name, message);
      return absl::OkStatus();
    };
    GOOGLESQL_RETURN_IF_ERROR(check(
        kDampingFactor,
        [](const Value& v) {
          return v.double_value() >= 0 && v.double_value() < 1;
        },
        "must be in the range [0, 1)"));
    for (absl::string_view name :
         {kMaxIterations, kMaxInnerIterations, kNumSourceNodes}) {
      GOOGLESQL_RETURN_IF_ERROR(check(
          name, [](const Value& v) { return v.int64_value() > 0; },
          "must be positive"));
    }
    GOOGLESQL_RETURN_IF_ERROR(check(
        kApproxPrecision, [](const Value& v) { return v.double_value() >= 0; },
        "must be non-negative"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kSampleSize, [](const Value& v) { return v.int64_value() >= 0; },
        "must be non-negative"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kResolution,
        [](const Value& v) {
          return std::isfinite(v.double_value()) && v.double_value() >= 0;
        },
        "must be finite and non-negative"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kEpsilon,
        [](const Value& v) {
          return v.double_value() > 0 && v.double_value() < 1;
        },
        "must be in the range (0, 1)"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kMinDensity,
        [](const Value& v) {
          return v.double_value() >= 0 && v.double_value() <= 1;
        },
        "must be in the range [0, 1]"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kMode,
        [](const Value& v) {
          return absl::EqualsIgnoreCase(v.string_value(), "EXACT") ||
                 absl::EqualsIgnoreCase(v.string_value(), "HYBRID");
        },
        "must be EXACT or HYBRID"));
    GOOGLESQL_RETURN_IF_ERROR(check(
        kMachineCategory,
        [](const Value& v) {
          return absl::EqualsIgnoreCase(v.string_value(), "default") ||
                 absl::EqualsIgnoreCase(v.string_value(), "large");
        },
        "must be default or large"));
    return check(
        kMaxIdleTime,
        [](const Value& v) {
          absl::Duration duration;
          return absl::ParseDuration(
                     absl::StrReplaceAll(v.string_value(), {{"µs", "us"}}),
                     &duration) &&
                 duration >= absl::ZeroDuration() &&
                 duration != absl::InfiniteDuration();
        },
        "must be a duration such as 4m, 1.5h or 1h45m");
  }

 private:
  const absl::string_view algorithm_;
  const absl::flat_hash_map<std::string, Value> values_;
};

// Serves precomputed rows.
class RowsIterator : public googlesql::EvaluatorTableIterator {
 public:
  RowsIterator(std::vector<TVFSchemaColumn> columns,
               std::vector<std::vector<Value>> rows)
      : columns_(std::move(columns)), rows_(std::move(rows)) {}

  int NumColumns() const override {
    return static_cast<int>(columns_.size());
  }
  std::string GetColumnName(int i) const override { return columns_[i].name; }
  const googlesql::Type* GetColumnType(int i) const override {
    return columns_[i].type;
  }
  const Value& GetValue(int i) const override { return rows_[row_][i]; }
  bool NextRow() override {
    return ++row_ < static_cast<int64_t>(rows_.size());
  }
  absl::Status Status() const override { return absl::OkStatus(); }
  absl::Status Cancel() override { return absl::OkStatus(); }

 private:
  const std::vector<TVFSchemaColumn> columns_;
  const std::vector<std::vector<Value>> rows_;
  int64_t row_ = -1;
};

class GraphAlgorithmTableValuedFunction
    : public googlesql::TableValuedFunction {
 public:
  GraphAlgorithmTableValuedFunction(AlgorithmSpec spec,
                                    googlesql::TypeFactory* type_factory)
      : googlesql::TableValuedFunction({std::string(spec.name)}, /*group=*/"",
                                       MakeSignatures(spec)),
        spec_(std::move(spec)),
        type_factory_(type_factory) {}

  absl::Status Resolve(
      const googlesql::AnalyzerOptions* analyzer_options,
      const std::vector<googlesql::TVFInputArgumentType>& actual_arguments,
      const FunctionSignature& concrete_signature, googlesql::Catalog* catalog,
      googlesql::TypeFactory* type_factory,
      std::shared_ptr<googlesql::TVFSignature>* output_tvf_signature)
      const override;

  absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
  CreateEvaluator(std::vector<TvfEvaluatorArg> input_arguments,
                  const std::vector<TVFSchemaColumn>& output_columns,
                  const FunctionSignature* function_call_signature)
      const override;

 private:
  const Parameter* FindParameter(absl::string_view name) const {
    for (const Parameter& parameter : spec_.parameters) {
      if (parameter.name == name) return &parameter;
    }
    return nullptr;
  }

  // Returns the output rows, one value per column of `OutputColumnNames()`.
  absl::StatusOr<std::vector<std::vector<Value>>> Run(
      const Arguments& arguments, const InputGraph& input,
      const googlesql::GraphPathType* path_type) const;

  // The node indexes of the graph nodes in the `name` argument, in argument
  // order and without duplicates. Nodes outside of the input are skipped.
  std::vector<int> NodeIndexes(const Arguments& arguments,
                               absl::string_view name,
                               const InputGraph& input) const;

  std::vector<std::string> OutputColumnNames() const;

  const AlgorithmSpec spec_;
  googlesql::TypeFactory* const type_factory_;
};

std::vector<std::string> GraphAlgorithmTableValuedFunction::OutputColumnNames()
    const {
  switch (spec_.output) {
    case Output::kScore:
      return {"node", "score"};
    case Output::kCentrality:
      return {"node", "centrality"};
    case Output::kCluster:
      return {"node", "cluster"};
    case Output::kClique:
      return {"node", "clique"};
    case Output::kSimilarity:
      return {"source_node", "target_node", "similarity"};
    case Output::kPath:
      return {"source_node", "target_node", "path", "cost"};
  }
  return {};
}

absl::Status GraphAlgorithmTableValuedFunction::Resolve(
    const googlesql::AnalyzerOptions* analyzer_options,
    const std::vector<googlesql::TVFInputArgumentType>& actual_arguments,
    const FunctionSignature& concrete_signature, googlesql::Catalog* catalog,
    googlesql::TypeFactory* type_factory,
    std::shared_ptr<googlesql::TVFSignature>* output_tvf_signature) const {
  GOOGLESQL_RET_CHECK(!actual_arguments.empty());
  const GraphElementType* node_type = nullptr;
  const GraphElementType* edge_type = nullptr;
  if (actual_arguments[0].is_graph()) {
    const googlesql::PropertyGraph* graph = actual_arguments[0].graph().graph();
    GOOGLESQL_ASSIGN_OR_RETURN(node_type,
                     ElementType(graph, kNodesExpression, type_factory));
    if (spec_.output == Output::kPath) {
      GOOGLESQL_ASSIGN_OR_RETURN(bool has_edge_tables, HasEdgeTables(graph));
      if (!has_edge_tables) {
        return absl::InvalidArgumentError(absl::StrCat(
            spec_.name, " requires a graph with at least one edge table"));
      }
      GOOGLESQL_ASSIGN_OR_RETURN(edge_type,
                       ElementType(graph, kEdgesExpression, type_factory));
    }
  } else {
    GOOGLESQL_RET_CHECK(actual_arguments[0].is_relation());
    GOOGLESQL_RETURN_IF_ERROR(RelationElementTypes(
        actual_arguments[0].relation(), spec_.name, node_type, edge_type));
    if (spec_.output == Output::kPath && edge_type == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat(
          spec_.name, ": the CALL PER () input must have an edge column"));
    }
  }

  for (int i = 1; i < concrete_signature.NumConcreteArguments(); ++i) {
    const std::string& name = concrete_signature.ConcreteArgument(i)
                                  .argument_name();
    if (actual_arguments[0].is_relation() &&
        (name == kNodeLabels || name == kEdgeLabels)) {
      return absl::InvalidArgumentError(absl::StrCat(
          spec_.name, ": ", name,
          " is only supported when CALL is used without PER ()"));
    }
    const Parameter* parameter = FindParameter(name);
    if (parameter == nullptr || parameter->type != ParameterType::kNodeArray) {
      continue;
    }
    GOOGLESQL_ASSIGN_OR_RETURN(googlesql::InputArgumentType argument_type,
                     actual_arguments[i].GetScalarArgType());
    const googlesql::Type* type = argument_type.type();
    if (type->IsArray() && type->AsArray()->element_type()->IsGraphElement() &&
        type->AsArray()->element_type()->AsGraphElement()->IsNode()) {
      continue;
    }
    // A NULL literal for an omitted argument has no node type.
    if (argument_type.is_literal_null()) continue;
    return absl::InvalidArgumentError(absl::StrCat(
        spec_.name, ": ", name, " must be an array of graph nodes, but is ",
        type->ShortTypeName(googlesql::PRODUCT_EXTERNAL)));
  }

  std::vector<googlesql::TVFRelation::Column> columns;
  const std::vector<std::string> names = OutputColumnNames();
  for (const std::string& name : names) {
    const googlesql::Type* type = nullptr;
    if (name == "node" || name == "source_node" || name == "target_node") {
      type = node_type;
    } else if (name == "cluster" || name == "clique") {
      type = googlesql::types::Int64Type();
    } else if (name == "path") {
      const googlesql::GraphPathType* path_type = nullptr;
      GOOGLESQL_RETURN_IF_ERROR(
          type_factory->MakeGraphPathType(node_type, edge_type, &path_type));
      type = path_type;
    } else {
      type = googlesql::types::DoubleType();
    }
    columns.emplace_back(name, type);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(*output_tvf_signature,
                   googlesql::TVFSignature::Create(
                       actual_arguments, googlesql::TVFRelation(columns)));
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<googlesql::EvaluatorTableIterator>>
GraphAlgorithmTableValuedFunction::CreateEvaluator(
    std::vector<TvfEvaluatorArg> input_arguments,
    const std::vector<TVFSchemaColumn>& output_columns,
    const FunctionSignature* function_call_signature) const {
  GOOGLESQL_RET_CHECK(function_call_signature != nullptr);
  GOOGLESQL_RET_CHECK_EQ(input_arguments.size(),
               function_call_signature->NumConcreteArguments());
  const googlesql::PropertyGraph* graph = nullptr;
  std::unique_ptr<googlesql::EvaluatorTableIterator> relation;
  absl::flat_hash_map<std::string, Value> values;
  for (int i = 0; i < input_arguments.size(); ++i) {
    TvfEvaluatorArg& argument = input_arguments[i];
    if (argument.graph != nullptr) {
      graph = argument.graph;
    } else if (argument.relation != nullptr) {
      relation = std::move(argument.relation);
    } else if (argument.value.has_value() && !argument.value->is_null()) {
      values.emplace(
          function_call_signature->ConcreteArgument(i).argument_name(),
          *std::move(argument.value));
    }
  }
  const Arguments arguments(spec_.name, std::move(values));
  GOOGLESQL_RETURN_IF_ERROR(arguments.Validate());

  InputGraph input;
  if (graph != nullptr) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Value> nodes,
                     ReadElements(graph, kNodesExpression, type_factory_));
    std::vector<Value> edges;
    GOOGLESQL_ASSIGN_OR_RETURN(bool has_edge_tables, HasEdgeTables(graph));
    if (has_edge_tables) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          edges, ReadElements(graph, kEdgesExpression, type_factory_));
    }
    if (const Value* labels = arguments.Find(kNodeLabels); labels != nullptr) {
      std::erase_if(nodes, [&](const Value& node) {
        return !HasAnyLabel(node, *labels);
      });
    }
    if (const Value* labels = arguments.Find(kEdgeLabels); labels != nullptr) {
      std::erase_if(edges, [&](const Value& edge) {
        return !HasAnyLabel(edge, *labels);
      });
    }
    input = MakeInputGraph(std::move(nodes), std::move(edges));
  } else {
    GOOGLESQL_RET_CHECK(relation != nullptr);
    GOOGLESQL_ASSIGN_OR_RETURN(input, ReadRelation(*relation));
  }

  const googlesql::GraphPathType* path_type = nullptr;
  for (const TVFSchemaColumn& column : output_columns) {
    if (column.type->IsGraphPath()) path_type = column.type->AsGraphPath();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::vector<Value>> rows,
                   Run(arguments, input, path_type));

  // Project the rows onto the selected output columns.
  const std::vector<std::string> names = OutputColumnNames();
  std::vector<int> projection;
  for (const TVFSchemaColumn& column : output_columns) {
    auto it = std::find(names.begin(), names.end(), column.name);
    GOOGLESQL_RET_CHECK(it != names.end()) << column.name;
    projection.push_back(static_cast<int>(it - names.begin()));
  }
  for (std::vector<Value>& row : rows) {
    std::vector<Value> projected;
    projected.reserve(projection.size());
    for (int index : projection) projected.push_back(std::move(row[index]));
    row = std::move(projected);
  }
  return std::make_unique<RowsIterator>(output_columns, std::move(rows));
}

std::vector<int> GraphAlgorithmTableValuedFunction::NodeIndexes(
    const Arguments& arguments, absl::string_view name,
    const InputGraph& input) const {
  std::vector<int> indexes;
  const Value* nodes = arguments.Find(name);
  if (nodes == nullptr) return indexes;
  absl::flat_hash_set<int> seen;
  for (const Value& node : nodes->elements()) {
    if (node.is_null()) continue;
    auto it = input.node_index.find(node.GetIdentifier());
    if (it != input.node_index.end() && seen.insert(it->second).second) {
      indexes.push_back(it->second);
    }
  }
  return indexes;
}

absl::StatusOr<std::vector<std::vector<Value>>>
GraphAlgorithmTableValuedFunction::Run(
    const Arguments& arguments, const InputGraph& input,
    const googlesql::GraphPathType* path_type) const {
  graph_algorithms::Graph graph;
  graph.num_nodes = static_cast<int>(input.nodes.size());
  const Value* weight_property = arguments.Find(kEdgeWeightProperty);
  for (const Value& edge : input.edges) {
    double weight = 1;
    if (weight_property != nullptr) {
      absl::StatusOr<Value> property =
          edge.FindPropertyByName(weight_property->string_value());
      if (!property.ok()) {
        return arguments.Error(
            kEdgeWeightProperty,
            absl::StrCat("names property ", weight_property->string_value(),
                         ", which edges of ", edge.GetDefinitionName(),
                         " do not have"));
      }
      if (!property->type()->IsNumerical()) {
        return arguments.Error(kEdgeWeightProperty,
                               "must name a numeric property");
      }
      // Edges without a weight get the default weight.
      if (!property->is_null()) weight = property->ToDouble();
      const bool allows_negative =
          spec_.algorithm == Algorithm::kCorrelationClustering;
      if (!std::isfinite(weight) || (weight < 0 && !allows_negative)) {
        return absl::InvalidArgumentError(absl::StrCat(
            spec_.name, ": edge weights must be finite and non-negative, but ",
            weight_property->string_value(), " is ", property->DebugString()));
      }
    }
    graph.edges.push_back({input.node_index.at(edge.GetSourceNodeIdentifier()),
                           input.node_index.at(edge.GetDestNodeIdentifier()),
                           weight});
  }

  std::vector<std::vector<Value>> rows;
  auto add_node_rows = [&](const auto& results, auto make_value) {
    for (int node = 0; node < graph.num_nodes; ++node) {
      rows.push_back({input.nodes[node], make_value(results[node])});
    }
  };
  auto similarity_rows = [&](graph_algorithms::Similarity similarity) {
    const graph_algorithms::NeighborhoodSimilarity neighborhoods(graph);
    for (int source : NodeIndexes(arguments, kSourceNodes, input)) {
      for (int target : NodeIndexes(arguments, kTargetNodes, input)) {
        rows.push_back(
            {input.nodes[source], input.nodes[target],
             Value::Double(neighborhoods.Compute(similarity, source, target))});
      }
    }
  };

  switch (spec_.algorithm) {
    case Algorithm::kPageRank:
      add_node_rows(
          graph_algorithms::PageRank(
              graph, NodeIndexes(arguments, kSourceNodes, input),
              arguments.Double(kDampingFactor, 0.85),
              arguments.Int64(kMaxIterations, 10),
              arguments.Double(kApproxPrecision, 1e-2)),
          Value::Double);
      break;
    case Algorithm::kBetweennessCentrality:
      add_node_rows(
          graph_algorithms::BetweennessCentrality(
              graph, arguments.Int64(kNumSourceNodes,
                                     std::numeric_limits<int64_t>::max())),
          Value::Double);
      break;
    case Algorithm::kClosenessCentrality:
      // HYBRID mode samples pivot nodes to approximate the centralities; on
      // emulator-sized graphs the exact values are computed instead.
      add_node_rows(graph_algorithms::ClosenessCentrality(
                        graph, arguments.Bool(kUseWassermanFaust, false)),
                    Value::Double);
      break;
    case Algorithm::kWeaklyConnectedComponents:
      add_node_rows(graph_algorithms::WeaklyConnectedComponents(graph),
                    Value::Int64);
      break;
    case Algorithm::kModularityClustering:
      add_node_rows(graph_algorithms::ModularityClustering(
                        graph, arguments.Double(kResolution, 1.0),
                        arguments.Int64(kMaxIterations, 10),
                        arguments.Int64(kMaxInnerIterations, 10)),
                    Value::Int64);
      break;
    case Algorithm::kCorrelationClustering: {
      const Value* resolution = arguments.Find(kResolution);
      if (resolution == nullptr) {
        return arguments.Error(kResolution, "must not be NULL");
      }
      add_node_rows(graph_algorithms::CorrelationClustering(
                        graph, resolution->double_value(),
                        arguments.Int64(kMaxIterations, 10),
                        arguments.Int64(kMaxInnerIterations, 10)),
                    Value::Int64);
      break;
    }
    case Algorithm::kLabelPropagation: {
      std::vector<int64_t> labels(graph.num_nodes);
      const Value* seed_property = arguments.Find(kSeedLabelProperty);
      for (int node = 0; node < graph.num_nodes; ++node) {
        labels[node] = node;
        if (seed_property == nullptr) continue;
        absl::StatusOr<Value> seed =
            input.nodes[node].FindPropertyByName(seed_property->string_value());
        if (!seed.ok()) continue;
        if (!seed->type()->IsInt64()) {
          return arguments.Error(kSeedLabelProperty,
                                 "must name an INT64 property");
        }
        if (!seed->is_null()) labels[node] = seed->int64_value();
      }
      add_node_rows(graph_algorithms::LabelPropagation(
                        graph, std::move(labels),
                        arguments.Int64(kMaxIterations, 10)),
                    Value::Int64);
      break;
    }
    case Algorithm::kCliqueFinding: {
      const std::vector<std::vector<int>> cliques =
          graph_algorithms::CliqueFinding(graph,
                                          arguments.Double(kMinDensity, 0.9));
      for (int clique = 0; clique < cliques.size(); ++clique) {
        for (int node : cliques[clique]) {
          rows.push_back({input.nodes[node], Value::Int64(clique)});
        }
      }
      break;
    }
    case Algorithm::kJaccardSimilarity:
      similarity_rows(graph_algorithms::Similarity::kJaccard);
      break;
    case Algorithm::kCosineSimilarity:
      similarity_rows(graph_algorithms::Similarity::kCosine);
      break;
    case Algorithm::kCommonNeighborsSimilarity:
      similarity_rows(graph_algorithms::Similarity::kCommonNeighbors);
      break;
    case Algorithm::kTotalNeighborsSimilarity:
      similarity_rows(graph_algorithms::Similarity::kTotalNeighbors);
      break;
    case Algorithm::kShortestPath: {
      const std::vector<int> targets =
          NodeIndexes(arguments, kTargetNodes, input);
      for (int source : NodeIndexes(arguments, kSourceNodes, input)) {
        const graph_algorithms::ShortestPathTree tree =
            graph_algorithms::ShortestPaths(graph, source);
        for (int target : targets) {
          if (std::isinf(tree.cost[target])) continue;
          // Paths are only built when the query uses them; the placeholder is
          // projected away otherwise.
          Value path = Value::NullInt64();
          if (path_type != nullptr) {
            std::vector<Value> elements = {input.nodes[source]};
            for (int edge : graph_algorithms::PathEdges(graph, tree, target)) {
              elements.push_back(input.edges[edge]);
              elements.push_back(input.nodes[graph.edges[edge].target]);
            }
            GOOGLESQL_ASSIGN_OR_RETURN(
                path, Value::MakeGraphPath(path_type, std::move(elements)));
          }
          rows.push_back({input.nodes[source], input.nodes[target],
                          std::move(path), Value::Double(tree.cost[target])});
        }
      }
      break;
    }
  }
  return rows;
}

}  // namespace

bool CallsGraphAlgorithm(absl::string_view sql,
                         const googlesql::AnalyzerOptions& options) {
  if (!absl::StrContainsIgnoreCase(sql, "CALL")) return false;
  std::unique_ptr<googlesql::ParserOutput> parser_output;
  if (!googlesql::ParseStatement(sql, options.GetParserOptions(),
                                 &parser_output)
           .ok()) {
    return false;
  }
  std::vector<const googlesql::ASTNode*> calls;
  parser_output->statement()->GetDescendantSubtreesWithKinds(
      {googlesql::AST_GQL_NAMED_CALL}, &calls);
  const std::vector<AlgorithmSpec> specs = AlgorithmSpecs();
  for (const googlesql::ASTNode* call : calls) {
    const std::string name = call->GetAsOrDie<googlesql::ASTGqlNamedCall>()
                                 ->tvf_call()
                                 ->name()
                                 ->ToIdentifierPathString();
    for (const AlgorithmSpec& spec : specs) {
      if (absl::EqualsIgnoreCase(name, spec.name)) return true;
    }
  }
  return false;
}

void EnableGraphAlgorithmLanguageFeatures(
    googlesql::LanguageOptions& language) {
  language.EnableLanguageFeature(
      googlesql::FEATURE_SQL_GRAPH_EXPOSE_GRAPH_ELEMENT);
  language.EnableLanguageFeature(
      googlesql::FEATURE_SQL_GRAPH_SET_OPERATION_PROPAGATION_MODE);
}

void AddGraphAlgorithmFunctions(
    googlesql::TypeFactory* type_factory,
    CaseInsensitiveStringMap<std::unique_ptr<googlesql::TableValuedFunction>>&
        table_valued_functions) {
  for (AlgorithmSpec& spec : AlgorithmSpecs()) {
    auto function = std::make_unique<GraphAlgorithmTableValuedFunction>(
        std::move(spec), type_factory);
    table_valued_functions.insert({function->FullName(), std::move(function)});
  }
}

}  // namespace google::spanner::emulator::backend
