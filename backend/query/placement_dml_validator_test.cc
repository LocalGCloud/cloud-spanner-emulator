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

#include "backend/query/placement_dml_validator.h"

#include <memory>
#include <optional>
#include <string>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "backend/access/write.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/schema.h"
#include "common/errors.h"
#include "tests/common/row_reader.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::googlesql::values::Int64;
using ::googlesql::values::String;
using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::HasSubstr;
using ::testing::Optional;

// Counts the mutations written by DML statements.
class CountingRowWriter : public RowWriter {
 public:
  absl::Status Write(const Mutation& m) override {
    ++writes_;
    return absl::OkStatus();
  }
  int writes() const { return writes_; }

 private:
  int writes_ = 0;
};

class PlacementDmlValidatorTest : public testing::Test {
 protected:
  void SetUp() override {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        schema_,
        test::CreateSchemaFromDDL(
            {
                R"(CREATE PLACEMENT europe OPTIONS (
                     instance_partition = 'europe-partition'))",
                R"(CREATE TABLE Singers (
                     SingerId INT64 NOT NULL,
                     Name STRING(MAX),
                     Location STRING(MAX) NOT NULL PLACEMENT KEY
                   ) PRIMARY KEY (SingerId))",
                R"(CREATE TABLE Labels (
                     LabelId INT64 NOT NULL,
                     Name STRING(MAX)
                   ) PRIMARY KEY (LabelId))",
            },
            &type_factory_));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        plain_schema_,
        test::CreateSchemaFromDDL({R"(CREATE TABLE Labels (
                                         LabelId INT64 NOT NULL,
                                         Name STRING(MAX)
                                       ) PRIMARY KEY (LabelId))"},
                                  &type_factory_));
    query_engine_ = std::make_unique<QueryEngine>(&type_factory_, schema_.get());
  }

  // Runs `sql` as a statement of a read-write transaction. When `restrictions`
  // is set, the placement DML restrictions apply.
  absl::StatusOr<QueryResult> Execute(
      const std::string& sql,
      std::optional<PlacementDmlRestrictions> restrictions =
          PlacementDmlRestrictions{},
      v1::ExecuteSqlRequest_QueryMode mode = v1::ExecuteSqlRequest::NORMAL) {
    return query_engine_->ExecuteSql(
        Query{sql},
        QueryContext{.schema = schema_.get(),
                     .reader = &reader_,
                     .writer = &writer_,
                     .allow_read_write_only_functions = true,
                     .is_read_only_txn = false,
                     .placement_dml_restrictions = restrictions},
        mode);
  }

  static absl::Status NonKeyWhereError(const std::string& column) {
    return error::PlacementTableNonKeyColumnInWhereClause("Singers", column);
  }

  googlesql::TypeFactory type_factory_;
  std::unique_ptr<const Schema> schema_;
  std::unique_ptr<const Schema> plain_schema_;
  std::unique_ptr<QueryEngine> query_engine_;
  CountingRowWriter writer_;
  test::TestRowReader reader_{
      {{"Singers",
        {{"SingerId", "Name", "Location"},
         {googlesql::types::Int64Type(), googlesql::types::StringType(),
          googlesql::types::StringType()},
         {{Int64(1), String("Marc"), String("europe")}}}},
       {"Labels",
        {{"LabelId", "Name"},
         {googlesql::types::Int64Type(), googlesql::types::StringType()},
         {{Int64(1), String("Label")}}}}}};
};

TEST_F(PlacementDmlValidatorTest, DetectsPlacementTables) {
  EXPECT_TRUE(HasPlacementTables(schema_.get()));
  EXPECT_TRUE(IsPlacementTable(schema_->FindTable("Singers")));
  EXPECT_FALSE(IsPlacementTable(schema_->FindTable("Labels")));
  EXPECT_FALSE(HasPlacementTables(plain_schema_.get()));
}

TEST_F(PlacementDmlValidatorTest, AllowsPrimaryKeyWhereClauses) {
  GOOGLESQL_EXPECT_OK(Execute("SELECT Name FROM Singers WHERE SingerId = 1"));
  GOOGLESQL_EXPECT_OK(Execute("UPDATE Singers SET Name = 'x' WHERE SingerId = 1"));
  // Moving a row to another placement by primary key is allowed.
  GOOGLESQL_EXPECT_OK(
      Execute("UPDATE Singers SET Location = 'europe' WHERE SingerId = 1"));
  GOOGLESQL_EXPECT_OK(Execute("DELETE FROM Singers WHERE SingerId = 1"));
  // Non-key columns may be selected, just not filtered on.
  GOOGLESQL_EXPECT_OK(Execute("SELECT Location, Name FROM Singers"));
}

TEST_F(PlacementDmlValidatorTest, RejectsNonKeyWhereClauses) {
  EXPECT_EQ(Execute("SELECT * FROM Singers WHERE Location = 'europe'").status(),
            NonKeyWhereError("Location"));
  EXPECT_EQ(Execute("SELECT * FROM Singers WHERE Name = 'Marc'").status(),
            NonKeyWhereError("Name"));
  EXPECT_EQ(Execute("UPDATE Singers SET Name = 'x' WHERE Location = 'europe'")
                .status(),
            NonKeyWhereError("Location"));
  EXPECT_EQ(
      Execute("DELETE FROM Singers WHERE SingerId = 1 AND Name = 'x'").status(),
      NonKeyWhereError("Name"));
  EXPECT_EQ(writer_.writes(), 0);
}

TEST_F(PlacementDmlValidatorTest, ChecksSubqueriesAndCorrelatedReferences) {
  EXPECT_EQ(Execute("DELETE FROM Labels WHERE LabelId IN "
                    "(SELECT SingerId FROM Singers WHERE Location = 'europe')")
                .status(),
            NonKeyWhereError("Location"));
  EXPECT_EQ(Execute("SELECT * FROM Labels l WHERE EXISTS (SELECT 1 FROM "
                    "Singers s WHERE s.SingerId = l.LabelId AND "
                    "s.Name = l.Name)")
                .status(),
            NonKeyWhereError("Name"));
  EXPECT_EQ(Execute("SELECT * FROM Singers WHERE Location IN "
                    "(SELECT Name FROM Labels)")
                .status(),
            NonKeyWhereError("Location"));
  // A subquery that only selects a non-key column doesn't reference it in the
  // enclosing WHERE clause.
  GOOGLESQL_EXPECT_OK(Execute("SELECT * FROM Labels WHERE Name IN "
                    "(SELECT UPPER(Name) FROM Singers)"));
}

TEST_F(PlacementDmlValidatorTest, DoesNotCheckJoinConditionsOrOtherTables) {
  GOOGLESQL_EXPECT_OK(Execute("SELECT s.SingerId FROM Singers s JOIN Labels l "
                    "ON s.Name = l.Name"));
  GOOGLESQL_EXPECT_OK(Execute("SELECT * FROM Labels WHERE Name = 'Label'"));
  GOOGLESQL_EXPECT_OK(Execute("UPDATE Labels SET Name = 'x' WHERE Name = 'Label'"));
}

TEST_F(PlacementDmlValidatorTest, RestrictionsApplyOnlyWhenRequested) {
  GOOGLESQL_EXPECT_OK(Execute("SELECT * FROM Singers WHERE Location = 'europe'",
                    /*restrictions=*/std::nullopt));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult result,
      Execute("INSERT INTO Singers (SingerId, Name, Location) "
              "VALUES (2, 'Ana', 'europe')",
              /*restrictions=*/std::nullopt));
  EXPECT_EQ(result.placement_sole_statement_table, std::nullopt);
}

TEST_F(PlacementDmlValidatorTest, ReportsPlacementInsertsAndDeletes) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult insert,
      Execute("INSERT INTO Singers (SingerId, Name, Location) "
              "VALUES (2, 'Ana', 'europe')"));
  EXPECT_THAT(insert.placement_sole_statement_table, Optional(std::string("Singers")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult upsert,
      Execute("INSERT OR UPDATE INTO Singers (SingerId, Name, Location) "
              "VALUES (2, 'Ana', 'europe')"));
  EXPECT_THAT(upsert.placement_sole_statement_table, Optional(std::string("Singers")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(QueryResult del,
                       Execute("DELETE FROM Singers WHERE SingerId = 1"));
  EXPECT_THAT(del.placement_sole_statement_table, Optional(std::string("Singers")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult update,
      Execute("UPDATE Singers SET Name = 'x' WHERE SingerId = 1"));
  EXPECT_EQ(update.placement_sole_statement_table, std::nullopt);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult other_table,
      Execute("INSERT INTO Labels (LabelId, Name) VALUES (2, 'x')"));
  EXPECT_EQ(other_table.placement_sole_statement_table, std::nullopt);
}

TEST_F(PlacementDmlValidatorTest, PlacementInsertMustBeOnlyStatement) {
  PlacementDmlRestrictions after_other_statements{
      .other_statements_in_transaction = true};
  EXPECT_EQ(Execute("INSERT INTO Singers (SingerId, Name, Location) "
                    "VALUES (2, 'Ana', 'europe')",
                    after_other_statements)
                .status(),
            error::PlacementDmlMustBeOnlyStatement("Singers"));
  EXPECT_EQ(
      Execute("DELETE FROM Singers WHERE SingerId = 1", after_other_statements)
          .status(),
      error::PlacementDmlMustBeOnlyStatement("Singers"));
  EXPECT_EQ(writer_.writes(), 0);

  // Other statements are unaffected.
  GOOGLESQL_EXPECT_OK(Execute("UPDATE Singers SET Name = 'x' WHERE SingerId = 1",
                    after_other_statements));
  GOOGLESQL_EXPECT_OK(Execute("INSERT INTO Labels (LabelId, Name) VALUES (2, 'x')",
                    after_other_statements));
}

TEST_F(PlacementDmlValidatorTest, PlanModeDoesNotExecuteOrReportStatements) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      QueryResult result,
      Execute("INSERT INTO Singers (SingerId, Name, Location) "
              "VALUES (2, 'Ana', 'europe')",
              PlacementDmlRestrictions{.other_statements_in_transaction = true},
              v1::ExecuteSqlRequest::PLAN));
  EXPECT_EQ(result.placement_sole_statement_table, std::nullopt);
  EXPECT_EQ(writer_.writes(), 0);
}

TEST_F(PlacementDmlValidatorTest, GetPlacementInsertOrDeleteTable) {
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"INSERT INTO Singers (SingerId, Name, Location) "
                        "VALUES (2, 'Ana', 'europe')"},
                  schema_.get()),
              IsOkAndHolds(Optional(std::string("Singers"))));
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"DELETE FROM Singers WHERE TRUE"}, schema_.get()),
              IsOkAndHolds(Optional(std::string("Singers"))));
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"UPDATE Singers SET Name = 'x' WHERE SingerId = 1"},
                  schema_.get()),
              IsOkAndHolds(std::nullopt));
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"INSERT INTO Labels (LabelId, Name) VALUES (2, 'x')"},
                  schema_.get()),
              IsOkAndHolds(std::nullopt));
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"INSERT INTO Labels (LabelId, Name) VALUES (2, 'x')"},
                  plain_schema_.get()),
              IsOkAndHolds(std::nullopt));
  EXPECT_THAT(query_engine_->GetPlacementInsertOrDeleteTable(
                  Query{"INSERT INTO NoSuchTable (x) VALUES (1)"},
                  schema_.get()),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("NoSuchTable")));
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
