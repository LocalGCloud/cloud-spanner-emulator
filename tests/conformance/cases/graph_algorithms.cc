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

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "tests/conformance/common/database_test_base.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using ::testing::HasSubstr;
using googlesql_base::testing::StatusIs;

// Runs the Spanner Graph algorithm catalog on the FinGraph sample graph of the
// Spanner Graph documentation: Accounts 7, 16 and 20 connected by Transfers,
// and Persons 1, 2 and 3 who each own one Account.
class GraphAlgorithmsTest : public DatabaseTest {
 public:
  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchema({
        R"(
          CREATE TABLE Person (
            id INT64 NOT NULL,
            name STRING(MAX),
          ) PRIMARY KEY (id)
        )",
        R"(
          CREATE TABLE Account (
            id INT64 NOT NULL,
            nick_name STRING(MAX),
            page_rank FLOAT64,
          ) PRIMARY KEY (id)
        )",
        R"(
          CREATE TABLE PersonOwnAccount (
            id INT64 NOT NULL,
            account_id INT64 NOT NULL,
            FOREIGN KEY (account_id) REFERENCES Account (id)
          ) PRIMARY KEY (id, account_id),
            INTERLEAVE IN PARENT Person ON DELETE CASCADE
        )",
        R"(
          CREATE TABLE AccountTransferAccount (
            id INT64 NOT NULL,
            to_id INT64 NOT NULL,
            amount FLOAT64,
            create_time TIMESTAMP NOT NULL,
            FOREIGN KEY (to_id) REFERENCES Account (id)
          ) PRIMARY KEY (id, to_id, create_time),
            INTERLEAVE IN PARENT Account ON DELETE CASCADE
        )",
        R"(
          CREATE PROPERTY GRAPH FinGraph
            NODE TABLES (Account, Person)
            EDGE TABLES (
              PersonOwnAccount
                SOURCE KEY (id) REFERENCES Person (id)
                DESTINATION KEY (account_id) REFERENCES Account (id)
                LABEL Owns,
              AccountTransferAccount
                SOURCE KEY (id) REFERENCES Account (id)
                DESTINATION KEY (to_id) REFERENCES Account (id)
                LABEL Transfers
            )
        )",
    }));
    return CommitDml({
                         R"(INSERT INTO Person (id, name)
                            VALUES (1, 'Alex'), (2, 'Dana'), (3, 'Lee'))",
                         R"(INSERT INTO Account (id, nick_name)
                            VALUES (7, 'Vacation Fund'), (16, 'Vacation Fund'),
                                   (20, 'Rainy Day Fund'))",
                         R"(INSERT INTO PersonOwnAccount (id, account_id)
                            VALUES (1, 7), (2, 20), (3, 16))",
                         R"(INSERT INTO AccountTransferAccount
                              (id, to_id, amount, create_time)
                            VALUES
                              (7, 16, 300, '2020-08-29T15:28:58.647Z'),
                              (7, 16, 100, '2020-10-04T16:55:05.342Z'),
                              (16, 20, 300, '2020-09-25T02:36:14.926Z'),
                              (20, 7, 500, '2020-10-04T16:55:05.342Z'),
                              (20, 16, 200, '2020-10-17T03:59:40.247Z'))",
                     })
        .status();
  }
};

TEST_F(GraphAlgorithmsTest, PageRankPersistedToSpanner) {
  // The "Run algorithm on subgraph defined by MATCH and persist results to
  // graph" example of the Spanner Graph documentation.
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        format = "CLOUD_SPANNER",
        table = "Account",
        write_mode = 'update_ignore_all'
      ) AS
      GRAPH FinGraph
      MATCH (n:Account)
      RETURN n
      FULL UNION ALL
      MATCH -[e:Transfers WHERE e.amount < 500]->
      RETURN e
      NEXT
      CALL PER () PageRank() YIELD node, score
      RETURN node.id, score AS page_rank)"),
              IsOkAndHoldsRows({}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      MATCH (n:Account)
      RETURN n.id, ROUND(n.page_rank, 2) AS page_rank
      ORDER BY page_rank DESC, id ASC)"),
              IsOkAndHoldsUnorderedRows({{20, 0.49}, {16, 0.46}, {7, 0.05}}));
}

TEST_F(GraphAlgorithmsTest, ExportDataToSpannerChecksTheDestination) {
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        format = "CLOUD_SPANNER", table = "Account",
        write_mode = 'update_ignore_all') AS
      GRAPH FinGraph
      CALL PageRank(node_labels => ['Account']) YIELD node, score
      RETURN score AS page_rank)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("must return the primary key column id")));
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        format = "CLOUD_SPANNER", table = "NoSuchTable",
        write_mode = 'update_ignore_all') AS
      GRAPH FinGraph
      CALL PageRank() YIELD node, score
      RETURN node.id, score)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("destination table not found")));
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        format = "CLOUD_SPANNER", table = "Account",
        write_mode = 'update_ignore_all') AS
      GRAPH FinGraph
      CALL PageRank(node_labels => ['Account']) YIELD node, score
      RETURN node.id, CAST(score AS STRING) AS page_rank)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("has type FLOAT64")));
}

TEST_F(GraphAlgorithmsTest, UpsertAddsMissingRows) {
  GOOGLESQL_ASSERT_OK(SetSchema({R"(
      CREATE TABLE AccountAlgoProperty (
        id INT64 NOT NULL,
        algo_run_id STRING(200) NOT NULL,
        int_val INT64,
      ) PRIMARY KEY (id, algo_run_id),
        INTERLEAVE IN PARENT Account)"}));
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        format = "CLOUD_SPANNER", table = "AccountAlgoProperty",
        write_mode = 'upsert_ignore_all') AS
      GRAPH FinGraph
      CALL WeaklyConnectedComponents(
        node_labels => ['Account'], edge_labels => ['Transfers'])
      YIELD node, cluster
      RETURN node.id, "wcc_1" AS algo_run_id, cluster AS int_val)"),
              IsOkAndHoldsRows({}));
  EXPECT_THAT(Query("SELECT id, algo_run_id, int_val FROM AccountAlgoProperty "
                    "ORDER BY id"),
              IsOkAndHoldsRows({{7, "wcc_1", 0}, {16, "wcc_1", 0},
                                {20, "wcc_1", 0}}));
}

TEST_F(GraphAlgorithmsTest, ExportDataToCloudStorageSucceedsWithoutRows) {
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (
        uri = "gs://my-bucket-name/my-output.csv",
        format = "csv"
      ) AS
      GRAPH FinGraph
      CALL WeaklyConnectedComponents(
        node_labels => ['Account'], edge_labels => ['Transfers'])
      YIELD node, cluster
      RETURN node.id, cluster)"),
              IsOkAndHoldsRows({}));
  // Argument errors are still reported.
  EXPECT_THAT(Query(R"(
      EXPORT DATA OPTIONS (uri = "gs://my-bucket-name/out.csv", format = "csv")
      AS GRAPH FinGraph
      CALL PageRank(damping_factor => 1) YIELD node, score
      RETURN node.id, score)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("damping_factor must be in the range")));
}

TEST_F(GraphAlgorithmsTest, Centrality) {
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL PageRank(node_labels => ['Account'], edge_labels => ['Transfers'])
      YIELD node, score
      RETURN node.id, CAST(ROUND(score * 1000000) AS INT64) AS score)"),
              IsOkAndHoldsUnorderedRows(
                  {{7, 214416}, {16, 398721}, {20, 386862}}));
  // Personalized PageRank from Account 7, as in the PageRank example.
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL PageRank(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        source_nodes => ARRAY {
          MATCH (n:Account {id:7})
          RETURN n
        },
        damping_factor => 0.5, max_iterations => 1
      ) YIELD node, score
      RETURN node.id, score)"),
              IsOkAndHoldsUnorderedRows({{7, 0.5}, {16, 0.5}, {20, 0.0}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL BetweennessCentrality(
        node_labels => ['Account'], edge_labels => ['Transfers']
      ) YIELD node, centrality
      RETURN node.id, centrality)"),
              IsOkAndHoldsUnorderedRows({{7, 0.0}, {16, 1.0}, {20, 1.0}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL ClosenessCentrality(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        mode => 'EXACT'
      ) YIELD node, centrality
      RETURN node.id, CAST(ROUND(centrality * 1000) AS INT64) AS centrality)"),
              IsOkAndHoldsUnorderedRows({{7, 667}, {16, 667}, {20, 1000}}));
}

TEST_F(GraphAlgorithmsTest, Clustering) {
  // The three Accounts are connected; the Persons are not connected by
  // Transfers.
  for (const std::string& algorithm :
       {"WeaklyConnectedComponents()", "ModularityClustering()",
        "CorrelationClustering(resolution => 0.5)",
        "LabelPropagation(max_iterations => 10)"}) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        auto rows,
        Query(absl::StrCat(
            "GRAPH FinGraph CALL ", algorithm,
            " YIELD node, cluster RETURN node.id, cluster, "
            "ELEMENT_DEFINITION_NAME(node) AS node_type")));
    EXPECT_EQ(rows.size(), 6) << algorithm;
  }
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL WeaklyConnectedComponents(
        node_labels => ['Person'], edge_labels => ['Transfers'])
      YIELD node, cluster
      RETURN node.id, cluster)"),
              IsOkAndHoldsUnorderedRows({{1, 0}, {2, 1}, {3, 2}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL ModularityClustering(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        resolution => 1.0, max_iterations => 10)
      YIELD node, cluster
      RETURN node.id, cluster)"),
              IsOkAndHoldsUnorderedRows({{7, 0}, {16, 0}, {20, 0}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL CorrelationClustering(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        resolution => 0.5)
      YIELD node, cluster
      RETURN node.id, cluster)"),
              IsOkAndHoldsUnorderedRows({{7, 0}, {16, 0}, {20, 0}}));
  // Labels start as node indexes; Account 16 (index 1) wins.
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL LabelPropagation(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        max_iterations => 10)
      YIELD node, cluster
      RETURN node.id, cluster)"),
              IsOkAndHoldsUnorderedRows({{7, 1}, {16, 1}, {20, 1}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL CliqueFinding(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        min_density => 0.9)
      YIELD node, clique
      RETURN node.id, clique)"),
              IsOkAndHoldsUnorderedRows({{7, 0}, {16, 0}, {20, 0}}));
}

TEST_F(GraphAlgorithmsTest, Similarity) {
  // The JaccardSimilarity example of the documentation. Account 7 neighbors
  // Accounts 16 and 20 and Person 1; each of the others shares one neighbor.
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL JaccardSimilarity(
        source_nodes => ARRAY {
          MATCH (n:Account {id: 7})
          RETURN n
        },
        target_nodes => ARRAY {
          MATCH (n:Account)
          WHERE n.id != 7
          RETURN n
        }
      ) YIELD source_node, target_node, similarity
      RETURN source_node.id AS source_id, target_node.id AS target_id,
             similarity)"),
              IsOkAndHoldsUnorderedRows({{7, 16, 0.2}, {7, 20, 0.2}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL CommonNeighborsSimilarity(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        source_nodes => ARRAY {MATCH (n:Account {id: 7}) RETURN n},
        target_nodes => ARRAY {MATCH (n:Account {id: 16}) RETURN n})
      YIELD source_node, target_node, similarity
      RETURN similarity)"),
              IsOkAndHoldsRows({{1.0}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL TotalNeighborsSimilarity(
        node_labels => ['Account'], edge_labels => ['Transfers'],
        source_nodes => ARRAY {MATCH (n:Account {id: 7}) RETURN n},
        target_nodes => ARRAY {MATCH (n:Account {id: 16}) RETURN n})
      YIELD source_node, target_node, similarity
      RETURN similarity)"),
              IsOkAndHoldsRows({{3.0}}));
}

TEST_F(GraphAlgorithmsTest, ShortestPath) {
  // The ShortestPath example of the documentation, with and without weights.
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL ShortestPath(
        source_nodes => ARRAY {
          MATCH (n:Account {id: 7})
          RETURN n
        },
        target_nodes => ARRAY {
          MATCH (n:Account {id: 20})
          RETURN n
        }
      ) YIELD source_node, target_node, path, cost
      RETURN source_node.id AS source_id, target_node.id AS target_id,
             PATH_LENGTH(path) AS length, cost)"),
              IsOkAndHoldsRows({{7, 20, 2, 2.0}}));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL ShortestPath(
        edge_weight_property => 'amount',
        source_nodes => ARRAY {MATCH (n:Account {id: 7}) RETURN n},
        target_nodes => ARRAY {MATCH (n:Account {id: 20}) RETURN n}
      ) YIELD source_node, target_node, path, cost
      RETURN PATH_LENGTH(path) AS length, cost)"),
              IsOkAndHoldsRows({{2, 400.0}}));
  // Persons cannot be reached from Accounts.
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL ShortestPath(
        source_nodes => ARRAY {MATCH (n:Account {id: 7}) RETURN n},
        target_nodes => ARRAY {MATCH (n:Person) RETURN n}
      ) YIELD source_node, target_node, path, cost
      RETURN cost)"),
              IsOkAndHoldsRows({}));
}

TEST_F(GraphAlgorithmsTest, InvalidCalls) {
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL CorrelationClustering(node_labels => ['Account'])
      YIELD node, cluster
      RETURN node.id, cluster)"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL CliqueFinding(min_density => 1.5) YIELD node, clique
      RETURN node.id, clique)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("min_density must be in the range [0, 1]")));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      CALL PageRank() YIELD node, score
      RETURN node, score)"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Query(R"(
      GRAPH FinGraph
      MATCH (n:Account) RETURN n
      NEXT
      CALL PER () PageRank(node_labels => ['Account']) YIELD node, score
      RETURN node.id, score)"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("only supported when CALL is used without "
                                 "PER ()")));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
