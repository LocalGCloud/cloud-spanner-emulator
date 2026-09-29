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

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/v1/query_plan.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "google/cloud/spanner/results.h"
#include "tests/conformance/common/database_test_base.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using googlesql_base::testing::StatusIs;
using ::testing::SizeIs;

class QueryModesTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

 public:
  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchemaFromFile("query_modes.test"));

    GOOGLESQL_RETURN_IF_ERROR(Insert("Users", {"ID", "Name"}, {1, "John"}).status());
    GOOGLESQL_RETURN_IF_ERROR(Insert("Users", {"ID", "Name"}, {2, "Peter"}).status());

    return absl::OkStatus();
  }
};

INSTANTIATE_TEST_SUITE_P(
    PerDialectQueryModesTest, QueryModesTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<QueryModesTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

// Returns the nodes of `plan` named `name`.
std::vector<const v1::PlanNode*> Nodes(const v1::QueryPlan& plan,
                                       absl::string_view name) {
  std::vector<const v1::PlanNode*> nodes;
  for (const v1::PlanNode& node : plan.plan_nodes()) {
    if (node.display_name() == name) {
      nodes.push_back(&node);
    }
  }
  return nodes;
}

// Returns the children of `node` linked with `type`.
std::vector<const v1::PlanNode*> Children(const v1::QueryPlan& plan,
                                          const v1::PlanNode& node,
                                          absl::string_view type) {
  std::vector<const v1::PlanNode*> children;
  for (const auto& link : node.child_links()) {
    if (link.type() == type) {
      children.push_back(&plan.plan_nodes(link.child_index()));
    }
  }
  return children;
}

std::string Metadata(const v1::PlanNode& node, const std::string& key) {
  auto it = node.metadata().fields().find(key);
  return it == node.metadata().fields().end() ? ""
                                              : it->second.string_value();
}

// Returns the value of the execution statistic `name`, e.g. rows.total.
std::string Stat(const v1::PlanNode& node, const std::string& name,
                 const std::string& field) {
  auto it = node.execution_stats().fields().find(name);
  if (it == node.execution_stats().fields().end()) {
    return "";
  }
  auto value = it->second.struct_value().fields().find(field);
  return value == it->second.struct_value().fields().end()
             ? ""
             : value->second.string_value();
}

// Plan nodes are listed in pre-order, and each node's index is its position.
void ExpectValidPlan(const v1::QueryPlan& plan) {
  ASSERT_GT(plan.plan_nodes_size(), 0);
  for (int i = 0; i < plan.plan_nodes_size(); ++i) {
    EXPECT_EQ(plan.plan_nodes(i).index(), i);
    for (const auto& link : plan.plan_nodes(i).child_links()) {
      EXPECT_GT(link.child_index(), i);
      EXPECT_LT(link.child_index(), plan.plan_nodes_size());
    }
  }
}

TEST_P(QueryModesTest, AcceptsQueriesInPlanMode) {
  // PLAN mode returns the query metadata without executing the statement.
  // This allows clients to execute AnalyzeSql to get the query metadata and
  // to let the backend infer the query parameters in a statement.
  auto plan = client().AnalyzeSql(Transaction(Transaction::ReadOnlyOptions()),
                                  SqlStatement("select * from Users"));
  ASSERT_TRUE(plan.ok());
  if (in_prod_env()) {
    return;
  }
  ExpectValidPlan(*plan);
  const v1::PlanNode& root = plan->plan_nodes(0);
  EXPECT_EQ(root.display_name(), "Serialize Result");
  EXPECT_EQ(root.kind(), v1::PlanNode::RELATIONAL);
  // A reference for each of the three output columns.
  EXPECT_THAT(Children(*plan, root, ""), SizeIs(4));
  ASSERT_THAT(Nodes(*plan, "Scan"), SizeIs(1));
  const v1::PlanNode& scan = *Nodes(*plan, "Scan")[0];
  EXPECT_EQ(Metadata(scan, "scan_type"), "TableScan");
  EXPECT_TRUE(absl::EqualsIgnoreCase(Metadata(scan, "scan_target"), "Users"));
  EXPECT_EQ(Metadata(scan, "Full scan"), "true");
  EXPECT_FALSE(scan.has_execution_stats());
}

TEST_P(QueryModesTest, PlansFiltersJoinsAggregatesAndSubqueries) {
  if (in_prod_env()) {
    GTEST_SKIP() << "Production plans are optimized.";
  }
  auto analyze = [&](const std::string& sql) -> v1::QueryPlan {
    auto plan = client().AnalyzeSql(
        Transaction(Transaction::ReadOnlyOptions()), SqlStatement(sql));
    EXPECT_TRUE(plan.ok()) << plan.status();
    if (!plan.ok()) {
      return v1::QueryPlan();
    }
    ExpectValidPlan(*plan);
    return *plan;
  };

  // A key lookup seeks.
  v1::QueryPlan plan = analyze("SELECT name FROM users WHERE id = 1");
  ASSERT_THAT(Nodes(plan, "Filter Scan"), SizeIs(1));
  const v1::PlanNode& filter_scan = *Nodes(plan, "Filter Scan")[0];
  EXPECT_EQ(Metadata(filter_scan, "seekable_key_size"), "1");
  ASSERT_THAT(Children(plan, filter_scan, "Seek Condition"), SizeIs(1));
  const v1::PlanNode& seek = *Children(plan, filter_scan, "Seek Condition")[0];
  EXPECT_EQ(seek.kind(), v1::PlanNode::SCALAR);
  EXPECT_EQ(seek.display_name(), "Function");
  EXPECT_TRUE(absl::StrContains(seek.short_representation().description(),
                                " = 1"));
  EXPECT_THAT(Children(plan, *Nodes(plan, "Distributed Union")[0],
                       "Split Range"),
              SizeIs(1));

  // Joins apply their map side to each row of their input side.
  plan = analyze(
      "SELECT a.name FROM users a JOIN users b ON a.id = b.age");
  ASSERT_THAT(Nodes(plan, "Cross Apply"), SizeIs(1));
  const v1::PlanNode& apply = *Nodes(plan, "Cross Apply")[0];
  EXPECT_THAT(Children(plan, apply, "Input"), SizeIs(1));
  EXPECT_THAT(Children(plan, apply, "Map"), SizeIs(1));
  EXPECT_THAT(Nodes(plan, "Scan"), SizeIs(2));

  // Sorting with a limit.
  plan = analyze(
      "SELECT name, COUNT(*) AS n FROM users GROUP BY name "
      "ORDER BY n DESC LIMIT 1");
  ASSERT_THAT(Nodes(plan, "Sort Limit"), SizeIs(1));
  const v1::PlanNode& sort_limit = *Nodes(plan, "Sort Limit")[0];
  EXPECT_THAT(Children(plan, sort_limit, "Key"), SizeIs(1));
  EXPECT_THAT(Children(plan, sort_limit, "Limit"), SizeIs(1));
  ASSERT_THAT(Nodes(plan, "Aggregate"), SizeIs(1));
  const v1::PlanNode& aggregate = *Nodes(plan, "Aggregate")[0];
  EXPECT_EQ(Metadata(aggregate, "iterator_type"), "Hash");
  ASSERT_THAT(Children(plan, aggregate, "Agg"), SizeIs(1));
  EXPECT_EQ(Children(plan, aggregate, "Agg")[0]
                ->short_representation()
                .description(),
            "COUNT(*)");

  // Subqueries are linked to the relational operator that evaluates them.
  plan = analyze(
      "SELECT name FROM users WHERE id IN "
      "(SELECT age FROM users WHERE name = 'John')");
  ASSERT_THAT(Nodes(plan, "In Subquery"), SizeIs(1));
  const v1::PlanNode& subquery = *Nodes(plan, "In Subquery")[0];
  EXPECT_THAT(subquery.short_representation().subqueries(), SizeIs(1));
  EXPECT_THAT(Nodes(plan, "Scan"), SizeIs(2));
}

TEST_P(QueryModesTest, PlansDmlWithoutExecutingIt) {
  auto plan = client().AnalyzeSql(
      Transaction(Transaction::ReadWriteOptions()),
      SqlStatement("UPDATE users SET name = 'Paul' WHERE id = 1"));
  ASSERT_TRUE(plan.ok()) << plan.status();
  EXPECT_THAT(Query("SELECT name FROM users WHERE id = 1"),
              IsOkAndHoldsRows({{"John"}}));
  if (in_prod_env()) {
    return;
  }
  ExpectValidPlan(*plan);
  const v1::PlanNode& root = plan->plan_nodes(0);
  EXPECT_EQ(root.display_name(), "Apply Mutations");
  EXPECT_EQ(Metadata(root, "operation_type"), "UPDATE");
  EXPECT_TRUE(absl::EqualsIgnoreCase(Metadata(root, "table"), "Users"));
  EXPECT_THAT(Nodes(*plan, "Filter Scan"), SizeIs(1));
}

TEST_P(QueryModesTest, ProvidesStatsInProfileMode) {
  auto profile = client().ProfileQuery(SqlStatement("select * from Users"));
  std::vector<ValueRow> rows;
  for (const auto& row : profile) {
    GOOGLESQL_ASSERT_OK(ToUtilStatusOr(row));
    rows.push_back(*row);
  }
  EXPECT_THAT(rows, SizeIs(2));
  auto stats = profile.ExecutionStats();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ("2", stats.value()["rows_returned"]);
  EXPECT_EQ(1, stats.value().count("elapsed_time"));
  EXPECT_EQ(1, stats.value().count("cpu_time"));
  if (in_prod_env()) {
    return;
  }
  EXPECT_EQ("2", stats.value()["rows_scanned"]);
  EXPECT_TRUE(absl::EndsWith(stats.value()["elapsed_time"], " msecs"));

  auto plan = profile.ExecutionPlan();
  ASSERT_TRUE(plan.has_value());
  ExpectValidPlan(*plan);
  const v1::PlanNode& root = plan->plan_nodes(0);
  EXPECT_EQ(Stat(root, "rows", "total"), "2");
  EXPECT_EQ(Stat(root, "latency", "unit"), "msecs");
  EXPECT_NE(Stat(root, "cpu_time", "total"), "");
  ASSERT_THAT(Nodes(*plan, "Scan"), SizeIs(1));
  const v1::PlanNode& scan = *Nodes(*plan, "Scan")[0];
  EXPECT_EQ(Stat(scan, "rows", "total"), "2");
  EXPECT_EQ(Stat(scan, "scanned_rows", "total"), "2");
  EXPECT_EQ(Stat(scan, "execution_summary", "num_executions"), "1");
  // Every statistic is a string.
  for (const v1::PlanNode& node : plan->plan_nodes()) {
    for (const auto& [name, stat] : node.execution_stats().fields()) {
      for (const auto& [field, value] : stat.struct_value().fields()) {
        EXPECT_TRUE(value.has_string_value()) << name << "." << field;
      }
    }
  }
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
