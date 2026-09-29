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

#include <memory>
#include <string>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/spanner/v1/query_plan.pb.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/query/analyzer_options.h"
#include "backend/query/catalog.h"
#include "backend/query/function_catalog.h"
#include "backend/schema/catalog/schema.h"
#include "common/constants.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::testing::ElementsAre;
using ::testing::SizeIs;

class QueryPlanBuilderTest : public testing::Test {
 protected:
  void SetUp() override {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema_, test::CreateSchemaFromDDL(
                     {R"(
                        CREATE TABLE Singers (
                          SingerId INT64 NOT NULL,
                          FirstName STRING(MAX),
                        ) PRIMARY KEY (SingerId)
                      )",
                      R"(
                        CREATE TABLE Albums (
                          SingerId INT64 NOT NULL,
                          AlbumId INT64 NOT NULL,
                          Title STRING(MAX),
                        ) PRIMARY KEY (SingerId, AlbumId)
                      )"},
                     &type_factory_));
    function_catalog_ = std::make_unique<FunctionCatalog>(
        &type_factory_, kCloudSpannerEmulatorFunctionCatalogName,
        schema_.get());
    analyzer_options_ = MakeGoogleSqlAnalyzerOptions(kDefaultTimeZone);
    analyzer_options_.set_prune_unused_columns(true);
    catalog_ = std::make_unique<Catalog>(schema_.get(), function_catalog_.get(),
                                         &type_factory_, analyzer_options_);
  }

  // Analyzes `sql` and returns its plan.
  v1::QueryPlan Plan(absl::string_view sql,
                     const StatementProfile* profile = nullptr) {
    GOOGLESQL_EXPECT_OK(googlesql::AnalyzeStatement(sql, analyzer_options_,
                                             catalog_.get(), &type_factory_,
                                             &output_));
    v1::QueryPlan plan =
        BuildQueryPlan(*output_->resolved_statement(), profile);
    ExpectPreOrder(plan);
    return plan;
  }

  // Nodes are listed in pre-order and each node's index is its position.
  static void ExpectPreOrder(const v1::QueryPlan& plan) {
    for (int i = 0; i < plan.plan_nodes_size(); ++i) {
      EXPECT_EQ(plan.plan_nodes(i).index(), i);
      for (const auto& link : plan.plan_nodes(i).child_links()) {
        EXPECT_GT(link.child_index(), i);
        EXPECT_LT(link.child_index(), plan.plan_nodes_size());
      }
    }
  }

  static std::vector<const v1::PlanNode*> Nodes(const v1::QueryPlan& plan,
                                                absl::string_view name) {
    std::vector<const v1::PlanNode*> nodes;
    for (const v1::PlanNode& node : plan.plan_nodes()) {
      if (node.display_name() == name) {
        nodes.push_back(&node);
      }
    }
    return nodes;
  }

  // Returns the child of `node` linked with `type`.
  static const v1::PlanNode& Child(const v1::QueryPlan& plan,
                                   const v1::PlanNode& node,
                                   absl::string_view type) {
    for (const auto& link : node.child_links()) {
      if (link.type() == type) {
        return plan.plan_nodes(link.child_index());
      }
    }
    ADD_FAILURE() << "No " << type << " child of " << node.display_name();
    return node;
  }

  static std::string Metadata(const v1::PlanNode& node,
                              const std::string& key) {
    auto it = node.metadata().fields().find(key);
    return it == node.metadata().fields().end() ? ""
                                                : it->second.string_value();
  }

  googlesql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_;
  std::unique_ptr<FunctionCatalog> function_catalog_;
  googlesql::AnalyzerOptions analyzer_options_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<const googlesql::AnalyzerOutput> output_;
};

TEST_F(QueryPlanBuilderTest, SeeksOnPrimaryKey) {
  v1::QueryPlan plan =
      Plan("SELECT FirstName FROM Singers WHERE SingerId = 1");

  const v1::PlanNode& root = plan.plan_nodes(0);
  EXPECT_EQ(root.display_name(), "Serialize Result");
  EXPECT_EQ(root.kind(), v1::PlanNode::RELATIONAL);
  EXPECT_EQ(Metadata(root, "execution_method"), "Row");
  ASSERT_THAT(root.child_links(), SizeIs(2));
  const v1::PlanNode& output =
      plan.plan_nodes(root.child_links(1).child_index());
  EXPECT_EQ(output.kind(), v1::PlanNode::SCALAR);
  EXPECT_EQ(output.short_representation().description(), "$FirstName");

  const v1::PlanNode& distributed_union =
      plan.plan_nodes(root.child_links(0).child_index());
  EXPECT_EQ(distributed_union.display_name(), "Distributed Union");
  EXPECT_EQ(Metadata(distributed_union, "distribution_table"), "Singers");
  EXPECT_EQ(Child(plan, distributed_union, "Split Range")
                .short_representation()
                .description(),
            "($SingerId = 1)");

  ASSERT_THAT(Nodes(plan, "Filter Scan"), SizeIs(1));
  const v1::PlanNode& filter_scan = *Nodes(plan, "Filter Scan")[0];
  EXPECT_EQ(Metadata(filter_scan, "seekable_key_size"), "1");
  EXPECT_EQ(Metadata(distributed_union, "subquery_cluster_node"),
            std::to_string(filter_scan.index()));
  const v1::PlanNode& seek = Child(plan, filter_scan, "Seek Condition");
  EXPECT_EQ(seek.display_name(), "Function");
  EXPECT_EQ(seek.short_representation().description(), "($SingerId = 1)");
  ASSERT_THAT(seek.child_links(), SizeIs(2));
  EXPECT_EQ(plan.plan_nodes(seek.child_links(0).child_index()).display_name(),
            "Reference");
  EXPECT_EQ(plan.plan_nodes(seek.child_links(1).child_index()).display_name(),
            "Constant");

  ASSERT_THAT(Nodes(plan, "Scan"), SizeIs(1));
  const v1::PlanNode& scan = *Nodes(plan, "Scan")[0];
  EXPECT_EQ(Metadata(scan, "scan_type"), "TableScan");
  EXPECT_EQ(Metadata(scan, "scan_target"), "Singers");
  EXPECT_EQ(Metadata(scan, "Full scan"), "");
  std::vector<std::string> variables;
  for (const auto& link : scan.child_links()) {
    variables.push_back(link.variable());
  }
  EXPECT_THAT(variables, ElementsAre("SingerId", "FirstName"));
}

TEST_F(QueryPlanBuilderTest, ScansTheWholeTableWithoutKeyConditions) {
  v1::QueryPlan plan = Plan("SELECT * FROM Singers");
  EXPECT_THAT(Nodes(plan, "Filter Scan"), SizeIs(0));
  ASSERT_THAT(Nodes(plan, "Scan"), SizeIs(1));
  EXPECT_EQ(Metadata(*Nodes(plan, "Scan")[0], "Full scan"), "true");

  plan = Plan("SELECT * FROM Albums WHERE Title = 'x' AND AlbumId > 2");
  ASSERT_THAT(Nodes(plan, "Filter Scan"), SizeIs(1));
  const v1::PlanNode& filter_scan = *Nodes(plan, "Filter Scan")[0];
  EXPECT_EQ(Metadata(filter_scan, "seekable_key_size"), "");
  int residuals = 0;
  for (const auto& link : filter_scan.child_links()) {
    residuals += link.type() == "Residual Condition";
    EXPECT_NE(link.type(), "Seek Condition");
  }
  EXPECT_EQ(residuals, 2);
  EXPECT_EQ(Metadata(*Nodes(plan, "Scan")[0], "Full scan"), "true");
}

TEST_F(QueryPlanBuilderTest, SeeksOnAKeyPrefix) {
  v1::QueryPlan plan = Plan(
      "SELECT Title FROM Albums WHERE SingerId = 1 AND AlbumId >= 2 AND "
      "Title IS NOT NULL");
  const v1::PlanNode& filter_scan = *Nodes(plan, "Filter Scan")[0];
  EXPECT_EQ(Metadata(filter_scan, "seekable_key_size"), "2");
  std::vector<std::string> conditions;
  for (const auto& link : filter_scan.child_links()) {
    if (!link.type().empty()) {
      conditions.push_back(absl::StrCat(
          link.type(), ": ",
          plan.plan_nodes(link.child_index())
              .short_representation()
              .description()));
    }
  }
  EXPECT_THAT(conditions,
              ElementsAre("Seek Condition: ($SingerId = 1)",
                          "Seek Condition: ($AlbumId >= 2)",
                          "Residual Condition: (NOT ($Title IS NULL))"));
}

TEST_F(QueryPlanBuilderTest, JoinsWithApplyOrHashJoin) {
  v1::QueryPlan plan = Plan(
      "SELECT s.FirstName, a.Title FROM Singers s JOIN Albums a "
      "ON s.SingerId = a.SingerId");
  ASSERT_THAT(Nodes(plan, "Cross Apply"), SizeIs(1));
  const v1::PlanNode& apply = *Nodes(plan, "Cross Apply")[0];
  EXPECT_EQ(Child(plan, apply, "Input").display_name(), "Distributed Union");
  const v1::PlanNode& map = Child(plan, apply, "Map");
  EXPECT_EQ(map.display_name(), "Filter");
  EXPECT_EQ(Child(plan, map, "Condition").short_representation().description(),
            "($SingerId = $SingerId)");
  EXPECT_THAT(Nodes(plan, "Scan"), SizeIs(2));

  plan = Plan(
      "SELECT s.FirstName FROM Singers s LEFT JOIN Albums a "
      "ON s.SingerId = a.SingerId");
  EXPECT_THAT(Nodes(plan, "Outer Apply"), SizeIs(1));

  plan = Plan(
      "SELECT s.FirstName FROM Singers s FULL JOIN Albums a "
      "ON s.SingerId = a.SingerId");
  ASSERT_THAT(Nodes(plan, "Hash Join"), SizeIs(1));
  const v1::PlanNode& hash_join = *Nodes(plan, "Hash Join")[0];
  EXPECT_EQ(Metadata(hash_join, "join_type"), "FULL");
  EXPECT_EQ(Child(plan, hash_join, "Build").display_name(),
            "Distributed Union");
  EXPECT_EQ(Child(plan, hash_join, "Probe").display_name(),
            "Distributed Union");
}

TEST_F(QueryPlanBuilderTest, AggregatesSortsAndLimits) {
  v1::QueryPlan plan = Plan(
      "SELECT SingerId, COUNT(*) AS albums FROM Albums GROUP BY SingerId "
      "ORDER BY albums DESC LIMIT 5");
  ASSERT_THAT(Nodes(plan, "Sort Limit"), SizeIs(1));
  const v1::PlanNode& sort_limit = *Nodes(plan, "Sort Limit")[0];
  EXPECT_TRUE(absl::EndsWith(
      Child(plan, sort_limit, "Key").short_representation().description(),
      " DESC"));
  EXPECT_EQ(
      Child(plan, sort_limit, "Limit").short_representation().description(),
      "5");

  ASSERT_THAT(Nodes(plan, "Aggregate"), SizeIs(1));
  const v1::PlanNode& aggregate = *Nodes(plan, "Aggregate")[0];
  EXPECT_EQ(Metadata(aggregate, "iterator_type"), "Hash");
  EXPECT_EQ(Child(plan, aggregate, "Agg").short_representation().description(),
            "COUNT(*)");
  EXPECT_EQ(Child(plan, aggregate, "Key").short_representation().description(),
            "$SingerId");

  plan = Plan("SELECT COUNT(DISTINCT Title) FROM Albums LIMIT 1");
  EXPECT_EQ(Metadata(*Nodes(plan, "Aggregate")[0], "iterator_type"), "Stream");
  EXPECT_THAT(Nodes(plan, "Limit"), SizeIs(1));
  EXPECT_THAT(Nodes(plan, "Sort Limit"), SizeIs(0));
}

TEST_F(QueryPlanBuilderTest, ComputesExpressionsAndUnions) {
  v1::QueryPlan plan = Plan(
      "SELECT SingerId + 1 AS next FROM Singers UNION ALL SELECT 1");
  ASSERT_THAT(Nodes(plan, "Union All"), SizeIs(1));
  EXPECT_THAT(Nodes(plan, "Union Input"), SizeIs(2));
  EXPECT_THAT(Nodes(plan, "Unit Relation"), SizeIs(1));
  ASSERT_THAT(Nodes(plan, "Compute"), SizeIs(2));
  const v1::PlanNode& compute = *Nodes(plan, "Compute")[0];
  EXPECT_EQ(compute.child_links(1).variable(), "next");
  EXPECT_EQ(plan.plan_nodes(compute.child_links(1).child_index())
                .short_representation()
                .description(),
            "($SingerId + 1)");
}

TEST_F(QueryPlanBuilderTest, LinksSubqueries) {
  v1::QueryPlan plan = Plan(
      "SELECT FirstName FROM Singers s WHERE EXISTS "
      "(SELECT 1 FROM Albums a WHERE a.SingerId = s.SingerId)");
  ASSERT_THAT(Nodes(plan, "Exists Subquery"), SizeIs(1));
  const v1::PlanNode& subquery = *Nodes(plan, "Exists Subquery")[0];
  EXPECT_EQ(subquery.kind(), v1::PlanNode::SCALAR);
  EXPECT_EQ(subquery.short_representation().description(), "EXISTS($sq_1)");
  EXPECT_EQ(subquery.short_representation().subqueries().at("sq_1"),
            subquery.index());
  EXPECT_EQ(plan.plan_nodes(subquery.child_links(0).child_index())
                .display_name(),
            "Compute");

  // The relational node that evaluates the subquery links to it.
  const v1::PlanNode& filter_scan = *Nodes(plan, "Filter Scan")[0];
  EXPECT_EQ(Child(plan, filter_scan, "Scalar").index(), subquery.index());
  EXPECT_THAT(Nodes(plan, "Scan"), SizeIs(2));
}

TEST_F(QueryPlanBuilderTest, AppliesMutationsForDml) {
  v1::QueryPlan plan =
      Plan("UPDATE Singers SET FirstName = 'x' WHERE SingerId = 1");
  const v1::PlanNode& root = plan.plan_nodes(0);
  EXPECT_EQ(root.display_name(), "Apply Mutations");
  EXPECT_EQ(Metadata(root, "operation_type"), "UPDATE");
  EXPECT_EQ(Metadata(root, "table"), "Singers");
  EXPECT_EQ(plan.plan_nodes(root.child_links(0).child_index()).display_name(),
            "Distributed Union");
  EXPECT_EQ(root.child_links(1).variable(), "FirstName");
  EXPECT_THAT(Nodes(plan, "Filter Scan"), SizeIs(1));

  plan = Plan("INSERT INTO Singers (SingerId, FirstName) VALUES (1, 'a')");
  EXPECT_EQ(Metadata(plan.plan_nodes(0), "operation_type"), "INSERT");
  EXPECT_EQ(plan.plan_nodes(1).display_name(), "Unit Relation");

  plan = Plan("DELETE FROM Albums WHERE true");
  EXPECT_EQ(Metadata(plan.plan_nodes(0), "operation_type"), "DELETE");
  EXPECT_EQ(Metadata(plan.plan_nodes(0), "table"), "Albums");
}

TEST_F(QueryPlanBuilderTest, ReportsProfilesAsStrings) {
  Plan("SELECT FirstName FROM Singers");
  std::vector<const googlesql::ResolvedNode*> scans;
  output_->resolved_statement()->GetDescendantsWithKinds(
      {googlesql::RESOLVED_TABLE_SCAN}, &scans);
  ASSERT_THAT(scans, SizeIs(1));
  StatementProfile profile{.rows = 3,
                           .latency = absl::Microseconds(1500),
                           .cpu_time = absl::Microseconds(1000)};
  profile.scans[scans[0]->GetAs<googlesql::ResolvedTableScan>()] = {
      .rows = 3, .executions = 1, .latency = absl::Microseconds(250)};

  v1::QueryPlan plan =
      BuildQueryPlan(*output_->resolved_statement(), &profile);
  const google::protobuf::Struct& root_stats =
      plan.plan_nodes(0).execution_stats();
  EXPECT_EQ(root_stats.fields().at("rows").struct_value().fields().at("total")
                .string_value(),
            "3");
  EXPECT_EQ(root_stats.fields().at("latency").struct_value().fields().at(
                "total").string_value(),
            "1.50");
  EXPECT_EQ(root_stats.fields().at("latency").struct_value().fields().at(
                "unit").string_value(),
            "msecs");
  EXPECT_EQ(root_stats.fields().at("cpu_time").struct_value().fields().at(
                "total").string_value(),
            "1.00");
  EXPECT_EQ(root_stats.fields()
                .at("execution_summary")
                .struct_value()
                .fields()
                .at("num_executions")
                .string_value(),
            "1");

  const google::protobuf::Struct& scan_stats =
      Nodes(plan, "Scan")[0]->execution_stats();
  EXPECT_EQ(scan_stats.fields().at("scanned_rows").struct_value().fields().at(
                "total").string_value(),
            "3");
  EXPECT_EQ(scan_stats.fields().at("latency").struct_value().fields().at(
                "total").string_value(),
            "0.25");
  EXPECT_FALSE(scan_stats.fields().contains("cpu_time"));
  // Other nodes are not measured.
  EXPECT_FALSE(Nodes(plan, "Distributed Union")[0]->has_execution_stats());
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
