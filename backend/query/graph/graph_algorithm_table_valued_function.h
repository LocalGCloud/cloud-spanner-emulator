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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHM_TABLE_VALUED_FUNCTION_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHM_TABLE_VALUED_FUNCTION_H_

#include <memory>

#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/strings/string_view.h"
#include "backend/common/case.h"

namespace google::spanner::emulator::backend {

// Registers the Spanner Graph algorithm catalog (PageRank,
// WeaklyConnectedComponents, ShortestPath, ...) as table-valued functions for
// GQL `CALL`. Each algorithm runs over the whole graph (`CALL Algo(...)`) or
// over the nodes and edges of the working table (`CALL PER () Algo(...)`),
// takes the documented named arguments and yields the documented columns.
// `type_factory` must outlive the functions; it owns the types of the graph
// elements that the functions read.
void AddGraphAlgorithmFunctions(
    googlesql::TypeFactory* type_factory,
    CaseInsensitiveStringMap<std::unique_ptr<googlesql::TableValuedFunction>>&
        table_valued_functions);

// Returns whether `sql` is a GoogleSQL statement that calls a graph algorithm.
bool CallsGraphAlgorithm(absl::string_view sql,
                         const googlesql::AnalyzerOptions& options);

// Enables the GoogleSQL features that the documented graph algorithm queries
// use beyond other graph queries: graph nodes as arguments, as in
// `source_nodes => ARRAY {MATCH (n) RETURN n}`, and working tables built with
// `FULL UNION ALL`.
void EnableGraphAlgorithmLanguageFeatures(googlesql::LanguageOptions& language);

}  // namespace google::spanner::emulator::backend

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHM_TABLE_VALUED_FUNCTION_H_
