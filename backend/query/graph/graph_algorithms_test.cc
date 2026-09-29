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

#include <cmath>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace google::spanner::emulator::backend::graph_algorithms {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Pointwise;

constexpr double kTolerance = 1e-9;

Graph MakeGraph(int num_nodes,
                std::vector<std::pair<int, int>> edges) {
  Graph graph{.num_nodes = num_nodes};
  for (const auto& [source, target] : edges) {
    graph.edges.push_back({source, target, 1.0});
  }
  return graph;
}

// Two directed triangles joined by the edge 2->3, and the isolated node 6.
Graph Bridged() {
  return MakeGraph(
      7, {{0, 1}, {1, 2}, {2, 0}, {2, 3}, {3, 4}, {4, 5}, {5, 3}});
}

// Two directed triangles and the isolated node 6.
Graph Disconnected() {
  return MakeGraph(7, {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}});
}

TEST(GraphAlgorithmsTest, PageRankMatchesDocumentedExample) {
  // The Transfers with amount < 500 between Accounts 7, 16 and 20 of the
  // Spanner Graph FinGraph sample; the documentation lists page ranks 0.05,
  // 0.46 and 0.49 after rounding.
  Graph graph = MakeGraph(3, {{0, 1}, {0, 1}, {1, 2}, {2, 1}});
  EXPECT_THAT(PageRank(graph, {}, 0.85, 10, 1e-2),
              Pointwise(DoubleNear(kTolerance),
                        {0.05, 0.456334550686556, 0.493665449313444}));
}

TEST(GraphAlgorithmsTest, PageRankStopsAtPrecision) {
  Graph graph = MakeGraph(2, {{0, 1}, {1, 0}});
  // The uniform start is already the stationary distribution.
  EXPECT_THAT(PageRank(graph, {}, 0.85, 100, 1e-2),
              Pointwise(DoubleNear(kTolerance), {0.5, 0.5}));
}

TEST(GraphAlgorithmsTest, PersonalizedPageRankTeleportsToSources) {
  // Node 2 is unreachable from the source and receives no rank.
  Graph graph = MakeGraph(3, {{0, 1}, {1, 0}, {2, 0}});
  std::vector<double> rank = PageRank(graph, {0}, 0.85, 50, 0);
  EXPECT_NEAR(rank[0] + rank[1], 1.0, kTolerance);
  EXPECT_NEAR(rank[2], 0.0, kTolerance);
  EXPECT_GT(rank[0], rank[1]);
}

TEST(GraphAlgorithmsTest, PageRankRedistributesDanglingRank) {
  Graph graph = MakeGraph(2, {{0, 1}});
  std::vector<double> rank = PageRank(graph, {}, 0.85, 100, 0);
  EXPECT_NEAR(rank[0] + rank[1], 1.0, kTolerance);
  EXPECT_GT(rank[1], rank[0]);
}

TEST(GraphAlgorithmsTest, BetweennessCentrality) {
  // Values from networkx.betweenness_centrality(normalized=False).
  EXPECT_THAT(BetweennessCentrality(Bridged(), 100),
              Pointwise(DoubleNear(kTolerance),
                        {1.0, 4.0, 7.0, 7.0, 4.0, 1.0, 0.0}));
}

TEST(GraphAlgorithmsTest, ApproximateBetweennessUsesFirstSources) {
  // Only node 0 is a source: paths 0->1->2 and 0->...->2->3... pass through
  // nodes 1..4, and the result is scaled by 7 / 1.
  EXPECT_THAT(BetweennessCentrality(Bridged(), 1),
              Pointwise(DoubleNear(kTolerance),
                        {0.0, 28.0, 21.0, 14.0, 7.0, 0.0, 0.0}));
}

TEST(GraphAlgorithmsTest, ClosenessCentrality) {
  // Values from networkx.closeness_centrality on the reversed graph, since
  // closeness here uses outgoing distances.
  EXPECT_THAT(
      ClosenessCentrality(Bridged(), /*use_wasserman_faust=*/false),
      Pointwise(DoubleNear(kTolerance),
                {5.0 / 15, 5.0 / 12, 5.0 / 9, 2.0 / 3, 2.0 / 3, 2.0 / 3, 0.0}));
  EXPECT_THAT(
      ClosenessCentrality(Bridged(), /*use_wasserman_faust=*/true),
      Pointwise(DoubleNear(kTolerance),
                {0.2777777777777778, 0.34722222222222227, 0.462962962962963,
                 2.0 / 9, 2.0 / 9, 2.0 / 9, 0.0}));
}

TEST(GraphAlgorithmsTest, WeaklyConnectedComponents) {
  EXPECT_THAT(WeaklyConnectedComponents(Disconnected()),
              ElementsAre(0, 0, 0, 1, 1, 1, 2));
  EXPECT_THAT(WeaklyConnectedComponents(Bridged()),
              ElementsAre(0, 0, 0, 0, 0, 0, 1));
}

TEST(GraphAlgorithmsTest, ModularityClusteringSplitsAtTheBridge) {
  EXPECT_THAT(ModularityClustering(Bridged(), 1.0, 10, 10),
              ElementsAre(0, 0, 0, 1, 1, 1, 2));
}

TEST(GraphAlgorithmsTest, ModularityWithZeroResolutionFindsComponents) {
  EXPECT_THAT(ModularityClustering(Bridged(), 0.0, 10, 10),
              ElementsAre(0, 0, 0, 0, 0, 0, 1));
}

TEST(GraphAlgorithmsTest, CorrelationClustering) {
  EXPECT_THAT(CorrelationClustering(Bridged(), 0.5, 10, 10),
              ElementsAre(0, 0, 0, 1, 1, 1, 2));
  EXPECT_THAT(CorrelationClustering(Bridged(), 0.0, 10, 10),
              ElementsAre(0, 0, 0, 0, 0, 0, 1));
  // With a high resolution no pair of nodes is worth putting together.
  EXPECT_THAT(CorrelationClustering(Bridged(), 5.0, 10, 10),
              ElementsAre(0, 1, 2, 3, 4, 5, 6));
}

TEST(GraphAlgorithmsTest, LabelPropagation) {
  EXPECT_THAT(LabelPropagation(Disconnected(), {0, 1, 2, 3, 4, 5, 6}, 10),
              ElementsAre(1, 1, 1, 4, 4, 4, 6));
  // Seed labels spread to the rest of their triangle.
  EXPECT_THAT(
      LabelPropagation(Disconnected(), {100, 100, 2, 200, 200, 5, 6}, 10),
      ElementsAre(100, 100, 100, 200, 200, 200, 6));
}

TEST(GraphAlgorithmsTest, CliqueFinding) {
  using Clusters = std::vector<std::vector<int>>;
  EXPECT_EQ(CliqueFinding(Bridged(), 1.0),
            (Clusters{{0, 1, 2}, {2, 3}, {3, 4, 5}, {6}}));
  EXPECT_EQ(CliqueFinding(Bridged(), 0.9),
            (Clusters{{0, 1, 2}, {2, 3}, {3, 4, 5}, {6}}));
  EXPECT_EQ(CliqueFinding(Bridged(), 0.6),
            (Clusters{{0, 1, 2, 3}, {3, 4, 5}, {6}}));
}

TEST(GraphAlgorithmsTest, NeighborhoodSimilarity) {
  const NeighborhoodSimilarity similarity(Bridged());
  // Node 0 has neighbors {1, 2}, node 1 has {0, 2}, node 3 has {2, 4, 5}.
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kJaccard, 0, 1), 1.0 / 3);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kCommonNeighbors, 0, 1), 1);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kTotalNeighbors, 0, 1), 3);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kCosine, 0, 1), 0.5);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kJaccard, 0, 3), 1.0 / 4);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kJaccard, 0, 6), 0);
  EXPECT_DOUBLE_EQ(similarity.Compute(Similarity::kCosine, 0, 6), 0);
}

TEST(GraphAlgorithmsTest, ShortestPaths) {
  Graph graph{.num_nodes = 4,
              .edges = {{0, 1, 1.0}, {1, 3, 1.0}, {0, 2, 0.5}, {2, 3, 0.5},
                        {0, 3, 5.0}}};
  ShortestPathTree tree = ShortestPaths(graph, 0);
  EXPECT_THAT(tree.cost,
              Pointwise(DoubleNear(kTolerance), {0.0, 1.0, 0.5, 1.0}));
  EXPECT_THAT(PathEdges(graph, tree, 3), ElementsAre(2, 3));
  EXPECT_THAT(PathEdges(graph, tree, 0), ElementsAre());

  ShortestPathTree from_three = ShortestPaths(graph, 3);
  EXPECT_TRUE(std::isinf(from_three.cost[0]));
}

}  // namespace
}  // namespace google::spanner::emulator::backend::graph_algorithms
