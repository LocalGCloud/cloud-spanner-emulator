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

#include "backend/query/graph/graph_algorithms.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"

namespace google::spanner::emulator::backend::graph_algorithms {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Gains below this threshold are treated as ties, so rounding noise does not
// make local search oscillate between equivalent moves.
constexpr double kGainEpsilon = 1e-12;

// Directed adjacency lists with parallel edges collapsed to the cheapest one
// and self-loops dropped, sorted by target.
std::vector<std::vector<std::pair<int, double>>> CheapestOutEdges(
    const Graph& graph) {
  std::vector<absl::btree_map<int, double>> cheapest(graph.num_nodes);
  for (const Graph::Edge& edge : graph.edges) {
    if (edge.source == edge.target) continue;
    auto [it, inserted] = cheapest[edge.source].try_emplace(edge.target,
                                                            edge.weight);
    if (!inserted) it->second = std::min(it->second, edge.weight);
  }
  std::vector<std::vector<std::pair<int, double>>> adjacency(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) {
    adjacency[node].assign(cheapest[node].begin(), cheapest[node].end());
  }
  return adjacency;
}

// Undirected adjacency with the weights of parallel edges in both directions
// summed, and the total self-loop weight of every node.
struct UndirectedGraph {
  std::vector<absl::btree_map<int, double>> neighbors;
  std::vector<double> self_loops;

  explicit UndirectedGraph(int num_nodes)
      : neighbors(num_nodes), self_loops(num_nodes, 0.0) {}

  int num_nodes() const { return static_cast<int>(neighbors.size()); }

  void AddEdge(int a, int b, double weight) {
    if (a == b) {
      self_loops[a] += weight;
      return;
    }
    neighbors[a][b] += weight;
    neighbors[b][a] += weight;
  }
};

UndirectedGraph ToUndirected(const Graph& graph) {
  UndirectedGraph undirected(graph.num_nodes);
  for (const Graph::Edge& edge : graph.edges) {
    undirected.AddEdge(edge.source, edge.target, edge.weight);
  }
  return undirected;
}

// Renumbers labels densely in order of first appearance.
std::vector<int64_t> DenseLabels(const std::vector<int64_t>& labels) {
  absl::flat_hash_map<int64_t, int64_t> dense;
  std::vector<int64_t> result;
  result.reserve(labels.size());
  for (int64_t label : labels) {
    result.push_back(dense.try_emplace(label, dense.size()).first->second);
  }
  return result;
}

// Dijkstra from `source` over `adjacency`, filling `distance`. Calls
// `visit(node)` for every reachable node in order of increasing distance, once
// its distance is final, and `relax(from, to, replaces)` for every edge that
// ends a shortest path to `to` found so far; `replaces` is set when the edge
// makes that path strictly shorter than the previous ones.
void Dijkstra(const std::vector<std::vector<std::pair<int, double>>>& adjacency,
              int source, std::vector<double>& distance,
              const std::function<void(int)>& visit,
              const std::function<void(int, int, bool)>& relax) {
  distance.assign(adjacency.size(), kInfinity);
  std::vector<bool> done(adjacency.size(), false);
  using Entry = std::pair<double, int>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  distance[source] = 0;
  queue.push({0, source});
  while (!queue.empty()) {
    auto [node_distance, node] = queue.top();
    queue.pop();
    if (done[node]) continue;
    done[node] = true;
    visit(node);
    for (const auto& [next, weight] : adjacency[node]) {
      if (done[next]) continue;
      const double next_distance = node_distance + weight;
      if (next_distance < distance[next]) {
        distance[next] = next_distance;
        relax(node, next, /*replaces=*/true);
        queue.push({next_distance, next});
      } else if (next_distance == distance[next]) {
        relax(node, next, /*replaces=*/false);
      }
    }
  }
}

// One level of Louvain-style local search. `gain(node, community, links)`
// returns the objective gain of adding `node` to `community`, given the total
// edge weight `links` between them, and `move(node, community, add)` updates
// the per-community state when `node` joins or leaves `community`. A node moves
// to the neighboring community with the largest gain or, when
// `allow_new_community` is set and every gain is negative, to an empty
// community. Returns whether any node moved.
template <typename GainFn, typename MoveFn>
bool LocalMoves(const UndirectedGraph& graph, int64_t max_passes,
                bool allow_new_community, const GainFn& gain,
                const MoveFn& move, std::vector<int>& community) {
  const int num_nodes = graph.num_nodes();
  community.resize(num_nodes);
  std::vector<int> num_members(num_nodes, 1);
  for (int node = 0; node < num_nodes; ++node) community[node] = node;
  // Communities without members. There is always one while a node is being
  // moved out of a community that keeps other members.
  absl::btree_set<int> empty_communities;
  bool any_moved = false;
  for (int64_t pass = 0; pass < max_passes; ++pass) {
    bool moved = false;
    for (int node = 0; node < num_nodes; ++node) {
      const int current = community[node];
      absl::btree_map<int, double> links;
      links[current] = 0;
      for (const auto& [neighbor, weight] : graph.neighbors[node]) {
        links[community[neighbor]] += weight;
      }
      move(node, current, /*add=*/false);
      if (--num_members[current] == 0) empty_communities.insert(current);
      int best = current;
      double best_gain = gain(node, current, links[current]);
      for (const auto& [candidate, weight] : links) {
        const double candidate_gain = gain(node, candidate, weight);
        if (candidate_gain > best_gain + kGainEpsilon) {
          best = candidate;
          best_gain = candidate_gain;
        }
      }
      if (allow_new_community && best_gain < -kGainEpsilon) {
        best = *empty_communities.begin();
      }
      move(node, best, /*add=*/true);
      if (num_members[best]++ == 0) empty_communities.erase(best);
      if (best != current) {
        community[node] = best;
        moved = true;
      }
    }
    if (!moved) break;
    any_moved = true;
  }
  return any_moved;
}

// Collapses every community of `graph` into a single node. Returns the
// community index of every node of `graph`, numbered densely in order of first
// appearance.
UndirectedGraph Aggregate(const UndirectedGraph& graph,
                          std::vector<int>& community) {
  absl::flat_hash_map<int, int> dense;
  for (int& c : community) {
    c = dense.try_emplace(c, static_cast<int>(dense.size())).first->second;
  }
  UndirectedGraph aggregated(static_cast<int>(dense.size()));
  for (int node = 0; node < graph.num_nodes(); ++node) {
    aggregated.self_loops[community[node]] += graph.self_loops[node];
    for (const auto& [neighbor, weight] : graph.neighbors[node]) {
      // Visit every undirected edge once.
      if (neighbor < node) continue;
      aggregated.AddEdge(community[node], community[neighbor], weight);
    }
  }
  return aggregated;
}

// Runs up to `max_iterations` levels of local moves followed by aggregation.
// `run_level(graph, node_sizes, community)` runs the local moves of a level,
// where `node_sizes` counts the original nodes behind every node of `graph`,
// and returns whether any node moved.
template <typename LevelFn>
std::vector<int64_t> MultilevelClustering(const Graph& graph,
                                          int64_t max_iterations,
                                          const LevelFn& run_level) {
  UndirectedGraph level_graph = ToUndirected(graph);
  // The level-graph node that holds every original node.
  std::vector<int> assignment(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) assignment[node] = node;
  std::vector<double> node_sizes(graph.num_nodes, 1.0);
  for (int64_t level = 0; level < max_iterations; ++level) {
    std::vector<int> community;
    if (!run_level(level_graph, node_sizes, community)) break;
    UndirectedGraph next = Aggregate(level_graph, community);
    std::vector<double> next_sizes(next.num_nodes(), 0.0);
    for (int node = 0; node < level_graph.num_nodes(); ++node) {
      next_sizes[community[node]] += node_sizes[node];
    }
    for (int& node : assignment) node = community[node];
    level_graph = std::move(next);
    node_sizes = std::move(next_sizes);
  }
  return DenseLabels(
      std::vector<int64_t>(assignment.begin(), assignment.end()));
}

}  // namespace

std::vector<double> PageRank(const Graph& graph,
                             const std::vector<int>& source_nodes,
                             double damping_factor, int64_t max_iterations,
                             double approx_precision) {
  const int num_nodes = graph.num_nodes;
  if (num_nodes == 0) return {};
  std::vector<double> teleport(num_nodes, 0.0);
  if (source_nodes.empty()) {
    std::fill(teleport.begin(), teleport.end(), 1.0 / num_nodes);
  } else {
    for (int node : source_nodes) teleport[node] = 1.0;
    const double num_sources =
        std::count(teleport.begin(), teleport.end(), 1.0);
    for (double& probability : teleport) probability /= num_sources;
  }
  std::vector<double> out_weight(num_nodes, 0.0);
  for (const Graph::Edge& edge : graph.edges) {
    out_weight[edge.source] += edge.weight;
  }

  std::vector<double> rank = teleport;
  std::vector<double> next(num_nodes);
  for (int64_t iteration = 0; iteration < max_iterations; ++iteration) {
    double dangling = 0;
    for (int node = 0; node < num_nodes; ++node) {
      if (out_weight[node] <= 0) dangling += rank[node];
    }
    for (int node = 0; node < num_nodes; ++node) {
      next[node] = (1 - damping_factor + damping_factor * dangling) *
                   teleport[node];
    }
    for (const Graph::Edge& edge : graph.edges) {
      if (out_weight[edge.source] <= 0) continue;
      next[edge.target] += damping_factor * rank[edge.source] * edge.weight /
                           out_weight[edge.source];
    }
    double change = 0;
    for (int node = 0; node < num_nodes; ++node) {
      change += std::abs(next[node] - rank[node]);
    }
    rank.swap(next);
    if (change < approx_precision) break;
  }
  return rank;
}

std::vector<double> BetweennessCentrality(const Graph& graph,
                                          int64_t num_source_nodes) {
  const int num_nodes = graph.num_nodes;
  const auto adjacency = CheapestOutEdges(graph);
  const int num_sources =
      static_cast<int>(std::min<int64_t>(num_source_nodes, num_nodes));
  std::vector<double> centrality(num_nodes, 0.0);
  std::vector<double> distance;
  for (int source = 0; source < num_sources; ++source) {
    std::vector<int> order;
    std::vector<std::vector<int>> predecessors(num_nodes);
    std::vector<double> num_paths(num_nodes, 0.0);
    num_paths[source] = 1;
    Dijkstra(
        adjacency, source, distance, [&](int node) { order.push_back(node); },
        [&](int from, int to, bool replaces) {
          if (replaces) {
            predecessors[to].clear();
            num_paths[to] = 0;
          }
          predecessors[to].push_back(from);
          num_paths[to] += num_paths[from];
        });
    std::vector<double> dependency(num_nodes, 0.0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const int node = *it;
      for (int predecessor : predecessors[node]) {
        dependency[predecessor] += num_paths[predecessor] / num_paths[node] *
                                   (1 + dependency[node]);
      }
      if (node != source) centrality[node] += dependency[node];
    }
  }
  if (num_sources > 0 && num_sources < num_nodes) {
    for (double& value : centrality) {
      value *= static_cast<double>(num_nodes) / num_sources;
    }
  }
  return centrality;
}

std::vector<double> ClosenessCentrality(const Graph& graph,
                                        bool use_wasserman_faust) {
  const int num_nodes = graph.num_nodes;
  const auto adjacency = CheapestOutEdges(graph);
  std::vector<double> centrality(num_nodes, 0.0);
  std::vector<double> distance;
  for (int source = 0; source < num_nodes; ++source) {
    int reachable = 0;
    double total_distance = 0;
    Dijkstra(
        adjacency, source, distance,
        [&](int node) {
          if (node == source) return;
          ++reachable;
          total_distance += distance[node];
        },
        [](int, int, bool) {});
    if (total_distance <= 0) continue;
    centrality[source] = reachable / total_distance;
    if (use_wasserman_faust) {
      centrality[source] *= static_cast<double>(reachable) / (num_nodes - 1);
    }
  }
  return centrality;
}

std::vector<int64_t> WeaklyConnectedComponents(const Graph& graph) {
  std::vector<int> parent(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) parent[node] = node;
  auto find = [&](int node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };
  for (const Graph::Edge& edge : graph.edges) {
    const int a = find(edge.source);
    const int b = find(edge.target);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
  std::vector<int64_t> roots(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) roots[node] = find(node);
  return DenseLabels(roots);
}

std::vector<int64_t> ModularityClustering(const Graph& graph,
                                          double resolution,
                                          int64_t max_iterations,
                                          int64_t max_inner_iterations) {
  return MultilevelClustering(
      graph, max_iterations,
      [&](const UndirectedGraph& level_graph, const std::vector<double>&,
          std::vector<int>& community) {
        // Weighted degrees; a self-loop adds twice its weight.
        std::vector<double> degree(level_graph.num_nodes(), 0.0);
        double total_degree = 0;
        for (int node = 0; node < level_graph.num_nodes(); ++node) {
          degree[node] = 2 * level_graph.self_loops[node];
          for (const auto& [neighbor, weight] : level_graph.neighbors[node]) {
            degree[node] += weight;
          }
          total_degree += degree[node];
        }
        if (total_degree <= 0) return false;
        std::vector<double> community_degree = degree;
        return LocalMoves(
            level_graph, max_inner_iterations, /*allow_new_community=*/false,
            [&](int node, int candidate, double links) {
              return links - resolution * community_degree[candidate] *
                                 degree[node] / total_degree;
            },
            [&](int node, int candidate, bool add) {
              community_degree[candidate] += add ? degree[node] : -degree[node];
            },
            community);
      });
}

std::vector<int64_t> CorrelationClustering(const Graph& graph,
                                           double resolution,
                                           int64_t max_iterations,
                                           int64_t max_inner_iterations) {
  return MultilevelClustering(
      graph, max_iterations,
      [&](const UndirectedGraph& level_graph,
          const std::vector<double>& node_sizes, std::vector<int>& community) {
        std::vector<double> community_size = node_sizes;
        return LocalMoves(
            level_graph, max_inner_iterations, /*allow_new_community=*/true,
            [&](int node, int candidate, double links) {
              return links -
                     resolution * node_sizes[node] * community_size[candidate];
            },
            [&](int node, int candidate, bool add) {
              community_size[candidate] +=
                  add ? node_sizes[node] : -node_sizes[node];
            },
            community);
      });
}

std::vector<int64_t> LabelPropagation(const Graph& graph,
                                      std::vector<int64_t> labels,
                                      int64_t max_iterations) {
  const UndirectedGraph undirected = ToUndirected(graph);
  for (int64_t iteration = 0; iteration < max_iterations; ++iteration) {
    bool changed = false;
    for (int node = 0; node < graph.num_nodes; ++node) {
      if (undirected.neighbors[node].empty()) continue;
      absl::btree_map<int64_t, double> votes;
      for (const auto& [neighbor, weight] : undirected.neighbors[node]) {
        votes[labels[neighbor]] += weight;
      }
      int64_t best = labels[node];
      auto own = votes.find(best);
      double best_votes = own == votes.end() ? -kInfinity : own->second;
      for (const auto& [label, weight] : votes) {
        if (weight > best_votes) {
          best = label;
          best_votes = weight;
        }
      }
      if (best != labels[node]) {
        labels[node] = best;
        changed = true;
      }
    }
    if (!changed) break;
  }
  return labels;
}

namespace {

using NodeSet = std::vector<int>;

NodeSet Intersect(const NodeSet& a, const NodeSet& b) {
  NodeSet result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(result));
  return result;
}

NodeSet Union(const NodeSet& a, const NodeSet& b) {
  NodeSet result;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                 std::back_inserter(result));
  return result;
}

// Bron-Kerbosch with Tomita pivoting. Appends every maximal clique that
// extends `clique` with nodes of `candidates` to `cliques`.
void MaximalCliques(const std::vector<NodeSet>& neighbors, NodeSet& clique,
                    NodeSet candidates, NodeSet excluded,
                    std::vector<NodeSet>& cliques) {
  if (candidates.empty()) {
    if (excluded.empty()) cliques.push_back(clique);
    return;
  }
  int pivot = -1;
  size_t pivot_degree = 0;
  for (const NodeSet* set : {&candidates, &excluded}) {
    for (int node : *set) {
      const size_t degree = Intersect(candidates, neighbors[node]).size();
      if (pivot < 0 || degree > pivot_degree) {
        pivot = node;
        pivot_degree = degree;
      }
    }
  }
  NodeSet branches;
  std::set_difference(candidates.begin(), candidates.end(),
                      neighbors[pivot].begin(), neighbors[pivot].end(),
                      std::back_inserter(branches));
  for (int node : branches) {
    clique.push_back(node);
    MaximalCliques(neighbors, clique, Intersect(candidates, neighbors[node]),
                   Intersect(excluded, neighbors[node]), cliques);
    clique.pop_back();
    candidates.erase(std::find(candidates.begin(), candidates.end(), node));
    excluded.insert(std::upper_bound(excluded.begin(), excluded.end(), node),
                    node);
  }
}

}  // namespace

std::vector<std::vector<int>> CliqueFinding(const Graph& graph,
                                            double min_density) {
  const UndirectedGraph undirected = ToUndirected(graph);
  std::vector<NodeSet> neighbors(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) {
    for (const auto& [neighbor, weight] : undirected.neighbors[node]) {
      neighbors[node].push_back(neighbor);
    }
  }
  std::vector<NodeSet> clusters;
  NodeSet clique;
  NodeSet all_nodes(graph.num_nodes);
  for (int node = 0; node < graph.num_nodes; ++node) all_nodes[node] = node;
  MaximalCliques(neighbors, clique, all_nodes, {}, clusters);
  for (NodeSet& cluster : clusters) std::sort(cluster.begin(), cluster.end());
  std::sort(clusters.begin(), clusters.end());

  auto num_edges = [&](const NodeSet& cluster) {
    int64_t count = 0;
    for (int node : cluster) {
      count += Intersect(neighbors[node], cluster).size();
    }
    return count / 2;
  };
  auto density = [&](const NodeSet& cluster) {
    const double size = cluster.size();
    return size < 2 ? 1.0 : num_edges(cluster) / (size * (size - 1) / 2);
  };
  auto connected = [&](const NodeSet& a, const NodeSet& b) {
    for (int node : a) {
      if (!Intersect(neighbors[node], b).empty() ||
          std::binary_search(b.begin(), b.end(), node)) {
        return true;
      }
    }
    return false;
  };
  while (true) {
    int best_a = -1;
    int best_b = -1;
    double best_density = -1;
    NodeSet best_union;
    for (int a = 0; a < static_cast<int>(clusters.size()); ++a) {
      for (int b = a + 1; b < static_cast<int>(clusters.size()); ++b) {
        if (!connected(clusters[a], clusters[b])) continue;
        NodeSet merged = Union(clusters[a], clusters[b]);
        const double merged_density = density(merged);
        if (merged_density >= min_density && merged_density > best_density) {
          best_a = a;
          best_b = b;
          best_density = merged_density;
          best_union = std::move(merged);
        }
      }
    }
    if (best_a < 0) break;
    clusters[best_a] = std::move(best_union);
    clusters.erase(clusters.begin() + best_b);
    // Drop clusters that the merge made redundant.
    std::vector<NodeSet> kept;
    for (int i = 0; i < static_cast<int>(clusters.size()); ++i) {
      const bool redundant =
          i != best_a && std::includes(clusters[best_a].begin(),
                                       clusters[best_a].end(),
                                       clusters[i].begin(), clusters[i].end());
      if (!redundant) kept.push_back(std::move(clusters[i]));
    }
    clusters = std::move(kept);
  }
  std::sort(clusters.begin(), clusters.end());
  return clusters;
}

NeighborhoodSimilarity::NeighborhoodSimilarity(const Graph& graph) {
  neighbors_ = ToUndirected(graph).neighbors;
}

double NeighborhoodSimilarity::Compute(Similarity similarity, int a,
                                       int b) const {
  const absl::btree_map<int, double>& first = neighbors_[a];
  const absl::btree_map<int, double>& second = neighbors_[b];
  double common = 0;
  double dot_product = 0;
  for (const auto& [node, weight] : first) {
    auto it = second.find(node);
    if (it == second.end()) continue;
    ++common;
    dot_product += weight * it->second;
  }
  const double total = first.size() + second.size() - common;
  switch (similarity) {
    case Similarity::kJaccard:
      return total == 0 ? 0 : common / total;
    case Similarity::kCommonNeighbors:
      return common;
    case Similarity::kTotalNeighbors:
      return total;
    case Similarity::kCosine: {
      double first_norm = 0;
      double second_norm = 0;
      for (const auto& [node, weight] : first) first_norm += weight * weight;
      for (const auto& [node, weight] : second) second_norm += weight * weight;
      if (first_norm == 0 || second_norm == 0) return 0;
      return dot_product / std::sqrt(first_norm * second_norm);
    }
  }
  return 0;
}

ShortestPathTree ShortestPaths(const Graph& graph, int source) {
  // Adjacency by edge index, so that paths can name the edges they use.
  std::vector<std::vector<int>> out_edges(graph.num_nodes);
  for (int edge = 0; edge < static_cast<int>(graph.edges.size()); ++edge) {
    out_edges[graph.edges[edge].source].push_back(edge);
  }
  ShortestPathTree tree{std::vector<double>(graph.num_nodes, kInfinity),
                        std::vector<int>(graph.num_nodes, -1)};
  std::vector<bool> done(graph.num_nodes, false);
  using Entry = std::pair<double, int>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  tree.cost[source] = 0;
  queue.push({0, source});
  while (!queue.empty()) {
    auto [cost, node] = queue.top();
    queue.pop();
    if (done[node]) continue;
    done[node] = true;
    for (int edge : out_edges[node]) {
      const int next = graph.edges[edge].target;
      const double next_cost = cost + graph.edges[edge].weight;
      if (!done[next] && next_cost < tree.cost[next]) {
        tree.cost[next] = next_cost;
        tree.parent_edge[next] = edge;
        queue.push({next_cost, next});
      }
    }
  }
  return tree;
}

std::vector<int> PathEdges(const Graph& graph, const ShortestPathTree& tree,
                           int target) {
  std::vector<int> path;
  for (int edge = tree.parent_edge[target]; edge >= 0;
       edge = tree.parent_edge[graph.edges[edge].source]) {
    path.push_back(edge);
  }
  std::reverse(path.begin(), path.end());
  return path;
}

}  // namespace google::spanner::emulator::backend::graph_algorithms
