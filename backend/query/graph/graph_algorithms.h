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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHMS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHMS_H_

#include <cstdint>
#include <vector>

#include "absl/container/btree_map.h"

// In-memory implementations of the Spanner Graph algorithm catalog. Production
// runs these algorithms on dedicated compute; the emulator runs deterministic
// single-threaded versions over the whole (small) graph. Every function visits
// nodes in index order and breaks ties towards the smallest index, so the same
// graph always produces the same result.
namespace google::spanner::emulator::backend::graph_algorithms {

// A directed multigraph over the nodes 0..num_nodes-1. Algorithms that work on
// undirected graphs ignore edge directions and sum the weights of parallel
// edges.
struct Graph {
  struct Edge {
    int source;
    int target;
    double weight;
  };
  int num_nodes = 0;
  std::vector<Edge> edges;
};

// PageRank by power iteration. The iteration starts from the teleport
// distribution, which is uniform over all nodes or, for personalized PageRank,
// over `source_nodes`. The rank of nodes without outgoing weight is
// redistributed along the teleport distribution. Stops after `max_iterations`
// iterations or once an iteration changes the ranks by less than
// `approx_precision` in L1 norm.
std::vector<double> PageRank(const Graph& graph,
                             const std::vector<int>& source_nodes,
                             double damping_factor, int64_t max_iterations,
                             double approx_precision);

// Brandes betweenness centrality over directed weighted shortest paths,
// without normalization. When `num_source_nodes` is smaller than the number of
// nodes, only the first `num_source_nodes` nodes are used as sources and the
// result is scaled by num_nodes / num_source_nodes.
std::vector<double> BetweennessCentrality(const Graph& graph,
                                          int64_t num_source_nodes);

// Closeness centrality over directed weighted shortest paths from each node:
// the number of nodes it reaches divided by the sum of the distances to them.
// The Wasserman-Faust variant further multiplies by the fraction of the other
// nodes that are reachable.
std::vector<double> ClosenessCentrality(const Graph& graph,
                                        bool use_wasserman_faust);

// Cluster IDs are dense, in the range [0, num_nodes), and numbered in the
// order in which clusters first appear in node order.
std::vector<int64_t> WeaklyConnectedComponents(const Graph& graph);

// Louvain modularity optimization on the undirected graph.
std::vector<int64_t> ModularityClustering(const Graph& graph,
                                          double resolution,
                                          int64_t max_iterations,
                                          int64_t max_inner_iterations);

// Louvain-style local search for the correlation clustering objective: the
// sum over pairs of nodes in the same cluster of their edge weight (zero for
// non-adjacent pairs) minus `resolution`.
std::vector<int64_t> CorrelationClustering(const Graph& graph,
                                           double resolution,
                                           int64_t max_iterations,
                                           int64_t max_inner_iterations);

// Asynchronous label propagation on the undirected graph, starting from
// `labels` (one per node). A node adopts the label with the largest total edge
// weight among its neighbors, keeping its own label on ties.
std::vector<int64_t> LabelPropagation(const Graph& graph,
                                      std::vector<int64_t> labels,
                                      int64_t max_iterations);

// Returns possibly overlapping clusters, each a sorted list of nodes. Starts
// from the maximal cliques of the undirected graph and greedily merges pairs
// of connected clusters while the merged cluster has at least `min_density`
// edge density, so every clique is contained in some cluster.
std::vector<std::vector<int>> CliqueFinding(const Graph& graph,
                                            double min_density);

enum class Similarity {
  kJaccard,
  kCosine,
  kCommonNeighbors,
  kTotalNeighbors,
};

// Pairwise node similarity based on undirected neighborhoods.
class NeighborhoodSimilarity {
 public:
  explicit NeighborhoodSimilarity(const Graph& graph);

  double Compute(Similarity similarity, int a, int b) const;

 private:
  // The neighbors of every node, with the total weight of the edges to them.
  std::vector<absl::btree_map<int, double>> neighbors_;
};

// Dijkstra shortest paths from a single source over directed edges with
// non-negative weights.
struct ShortestPathTree {
  // The cost of the cheapest path to every node, infinity when unreachable.
  std::vector<double> cost;
  // The index of the last edge on the cheapest path to every node, -1 for the
  // source and for unreachable nodes.
  std::vector<int> parent_edge;
};
ShortestPathTree ShortestPaths(const Graph& graph, int source);

// The indexes of the edges on the cheapest path to a reachable `target`, in
// path order.
std::vector<int> PathEdges(const Graph& graph, const ShortestPathTree& tree,
                           int target);

}  // namespace google::spanner::emulator::backend::graph_algorithms

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_GRAPH_GRAPH_ALGORITHMS_H_
