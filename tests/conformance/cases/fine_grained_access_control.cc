//
// Copyright 2026 Google LLC
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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/result_set.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"
#include "google/cloud/spanner/client.h"
#include "google/cloud/spanner/mutations.h"
#include "tests/common/change_streams.h"
#include "tests/conformance/common/database_test_base.h"
#include "grpcpp/client_context.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

// Matches the error for a database role that lacks the privileges an operation
// needs on an object of `object_kind`, such as "table" or "change stream".
auto LacksPrivileges(absl::string_view role, absl::string_view object_kind,
                     absl::string_view object_name) {
  return StatusIs(
      absl::StatusCode::kPermissionDenied,
      HasSubstr(absl::Substitute(
          "Role $0 does not have required privileges on $1 $2.", role,
          object_kind, object_name)));
}

// Matches the error for a session whose database role does not exist.
auto RoleNotFound(absl::string_view role) {
  return StatusIs(absl::StatusCode::kPermissionDenied,
                  HasSubstr(absl::StrCat("Role not found: ", role, ".")));
}

absl::Status ToStatus(const grpc::Status& status) {
  return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                      status.error_message());
}

// Tests fine-grained access control: the privileges of the database role that
// a session uses (Session.creator_role) govern its queries, DML, reads and
// mutations. Sessions without a role are not restricted.
class FineGrainedAccessControlTest
    : public DatabaseTest,
      public testing::WithParamInterface<database_api::DatabaseDialect> {
 public:
  void SetUp() override {
    dialect_ = GetParam();
    DatabaseTest::SetUp();
  }

  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchema({Ddl(
        R"(
          CREATE TABLE Singers (
            SingerId   INT64 NOT NULL,
            FirstName  STRING(1024),
            LastName   STRING(1024),
            SingerInfo BYTES(MAX)
          ) PRIMARY KEY (SingerId))",
        R"(
          CREATE TABLE Singers (
            SingerId   bigint NOT NULL,
            FirstName  varchar(1024),
            LastName   varchar(1024),
            SingerInfo bytea,
            PRIMARY KEY (SingerId)
          ))")}));
    return Insert(Name("Singers"), SingerColumns(), {1, "Marc", "Richards"})
        .status();
  }

 protected:
  bool is_postgresql() const {
    return dialect_ == database_api::DatabaseDialect::POSTGRESQL;
  }

  // Returns the text for the dialect of the test database.
  std::string Ddl(absl::string_view google_sql,
                  absl::string_view postgresql) const {
    return std::string(is_postgresql() ? postgresql : google_sql);
  }

  // Returns the name of an object as the database stores it. PostgreSQL folds
  // unquoted identifiers to lower case.
  std::string Name(absl::string_view name) const {
    return is_postgresql() ? absl::AsciiStrToLower(name) : std::string(name);
  }

  std::vector<std::string> SingerColumns() const {
    return {Name("SingerId"), Name("FirstName"), Name("LastName")};
  }

  // Returns a statement granting `what`, such as "SELECT ON TABLE Singers",
  // to `role`.
  std::string Grant(absl::string_view what, absl::string_view role) const {
    return absl::StrCat("GRANT ", what, Ddl(" TO ROLE ", " TO "), role);
  }

  std::string Revoke(absl::string_view what, absl::string_view role) const {
    return absl::StrCat("REVOKE ", what, Ddl(" FROM ROLE ", " FROM "), role);
  }

  // Returns a statement making `member` a member of `role`.
  std::string GrantRole(absl::string_view role,
                        absl::string_view member) const {
    return Ddl(absl::StrCat("GRANT ROLE ", role, " TO ROLE ", member),
               absl::StrCat("GRANT ", role, " TO ", member));
  }

  std::string RevokeRole(absl::string_view role,
                         absl::string_view member) const {
    return Ddl(absl::StrCat("REVOKE ROLE ", role, " FROM ROLE ", member),
               absl::StrCat("REVOKE ", role, " FROM ", member));
  }

  // Runs a read-only query in a session that uses `role`.
  absl::StatusOr<std::vector<ValueRow>> QueryAs(const std::string& role,
                                                const std::string& sql) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    auto rows = role_client->ExecuteQuery(SqlStatement(sql));
    GOOGLESQL_ASSIGN_OR_RETURN(ReadResult result,
                               ProcessRowStreamForReadResult(rows));
    return result.values;
  }

  // Runs a read-only query that returns one STRING column in a session that
  // uses `role`.
  absl::StatusOr<std::vector<std::string>> QueryStringsAs(
      const std::string& role, const std::string& sql) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<ValueRow> rows, QueryAs(role, sql));
    std::vector<std::string> strings;
    for (const ValueRow& row : rows) {
      if (row.values().size() != 1 ||
          !row.values()[0].get<std::string>().ok()) {
        return absl::InternalError("Expected a single STRING column");
      }
      strings.push_back(*row.values()[0].get<std::string>());
    }
    return strings;
  }

  // Runs a query or DML statement in a read-write transaction of a session that
  // uses `role` and commits it.
  absl::StatusOr<std::vector<ValueRow>> ReadWriteQueryAs(
      const std::string& role, const std::string& sql) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    std::vector<ValueRow> values;
    GOOGLESQL_RETURN_IF_ERROR(
        ToUtilStatusOr(role_client->Commit(
                           [&](const Transaction& txn)
                               -> cloud::StatusOr<Mutations> {
                             values.clear();
                             for (const auto& row : role_client->ExecuteQuery(
                                      txn, SqlStatement(sql))) {
                               if (!row.ok()) return row.status();
                               values.push_back(*row);
                             }
                             return Mutations{};
                           }))
            .status());
    return values;
  }

  // Executes a DML statement in a session that uses `role` and commits it.
  absl::Status ExecuteDmlAs(const std::string& role, const std::string& sql) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    return ToUtilStatusOr(
               role_client->Commit(
                   [&](const Transaction& txn) -> cloud::StatusOr<Mutations> {
                     auto result =
                         role_client->ExecuteDml(txn, SqlStatement(sql));
                     if (!result.ok()) return result.status();
                     return Mutations{};
                   }))
        .status();
  }

  // Reads all rows of `table` in a session that uses `role`.
  absl::StatusOr<std::vector<ValueRow>> ReadAs(
      const std::string& role, const std::string& table,
      std::vector<std::string> columns) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    auto rows =
        role_client->Read(table, KeySet::All(), std::move(columns));
    GOOGLESQL_ASSIGN_OR_RETURN(ReadResult result,
                               ProcessRowStreamForReadResult(rows));
    return result.values;
  }

  // Commits `mutations` in a session that uses `role`.
  absl::Status CommitAs(const std::string& role, Mutations mutations) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    return ToUtilStatusOr(role_client->Commit(std::move(mutations))).status();
  }

  // Applies `mutations` as one BatchWrite mutation group in a session that
  // uses `role`.
  absl::Status BatchWriteAs(const std::string& role, Mutations mutations) {
    std::unique_ptr<Client> role_client = MakeClientWithRole(role);
    std::vector<Mutations> mutation_groups;
    mutation_groups.push_back(std::move(mutations));
    for (const auto& result :
         role_client->CommitAtLeastOnce(std::move(mutation_groups))) {
      if (!result.ok()) {
        return ToUtilStatus(result.status());
      }
      if (!result->commit_timestamp.ok()) {
        return ToUtilStatus(result->commit_timestamp.status());
      }
    }
    return absl::OkStatus();
  }

  absl::StatusOr<spanner_api::Session> CreateSession(const std::string& role,
                                                     bool multiplexed = false) {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(database()->FullName());
    request.mutable_session()->set_creator_role(role);
    request.mutable_session()->set_multiplexed(multiplexed);
    spanner_api::Session response;
    GOOGLESQL_RETURN_IF_ERROR(
        ToStatus(raw_client()->CreateSession(&context, request, &response)));
    return response;
  }

  absl::StatusOr<std::vector<spanner_api::Session>> BatchCreateSessions(
      const std::string& role, int count) {
    grpc::ClientContext context;
    spanner_api::BatchCreateSessionsRequest request;
    request.set_database(database()->FullName());
    request.mutable_session_template()->set_creator_role(role);
    request.set_session_count(count);
    spanner_api::BatchCreateSessionsResponse response;
    GOOGLESQL_RETURN_IF_ERROR(ToStatus(
        raw_client()->BatchCreateSessions(&context, request, &response)));
    return std::vector<spanner_api::Session>(response.session().begin(),
                                             response.session().end());
  }

  absl::StatusOr<spanner_api::Session> GetSession(const std::string& name) {
    grpc::ClientContext context;
    spanner_api::GetSessionRequest request;
    request.set_name(name);
    spanner_api::Session response;
    GOOGLESQL_RETURN_IF_ERROR(
        ToStatus(raw_client()->GetSession(&context, request, &response)));
    return response;
  }

  // Returns the plan of a query in a session that uses `role`, without
  // executing the query.
  absl::Status PlanQueryAs(const std::string& role, const std::string& sql) {
    GOOGLESQL_ASSIGN_OR_RETURN(spanner_api::Session session,
                               CreateSession(role));
    grpc::ClientContext context;
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session.name());
    request.set_sql(sql);
    request.set_query_mode(spanner_api::ExecuteSqlRequest::PLAN);
    spanner_api::ResultSet response;
    return ToStatus(raw_client()->ExecuteSql(&context, request, &response));
  }

  // Runs a single-use read-only query in the session named `session`.
  absl::Status ExecuteSqlInSession(const std::string& session,
                                   const std::string& sql) {
    grpc::ClientContext context;
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql(sql);
    spanner_api::ResultSet response;
    return ToStatus(raw_client()->ExecuteSql(&context, request, &response));
  }

  // Returns the names of the database roles that ListDatabaseRoles returns,
  // without the database prefix.
  absl::StatusOr<std::vector<std::string>> ListDatabaseRoleNames() {
    const std::string prefix =
        absl::StrCat(database()->FullName(), "/databaseRoles/");
    std::vector<std::string> names;
    std::string page_token;
    do {
      grpc::ClientContext context;
      database_api::ListDatabaseRolesRequest request;
      request.set_parent(database()->FullName());
      request.set_page_token(page_token);
      database_api::ListDatabaseRolesResponse response;
      GOOGLESQL_RETURN_IF_ERROR(ToStatus(raw_database_client()->ListDatabaseRoles(
          &context, request, &response)));
      for (const database_api::DatabaseRole& role : response.database_roles()) {
        if (!absl::StartsWith(role.name(), prefix)) {
          return absl::InternalError(
              absl::StrCat("Unexpected database role name: ", role.name()));
        }
        std::string name = role.name().substr(prefix.size());
        // Production Spanner may also list this system role.
        if (name != "spanner_secure_context_reader") {
          names.push_back(std::move(name));
        }
      }
      page_token = response.next_page_token();
    } while (!page_token.empty());
    return names;
  }

  // Runs the initial query of the change stream `name` in a session that uses
  // `role` and returns the partition tokens it reports.
  absl::StatusOr<std::vector<std::string>> ReadChangeStreamAs(
      const std::string& role, const std::string& name) {
    GOOGLESQL_ASSIGN_OR_RETURN(spanner_api::Session session,
                               CreateSession(role));
    return GetActiveTokenFromInitialQuery(dialect_, absl::Now(), name,
                                          session.name(), raw_client());
  }
};

INSTANTIATE_TEST_SUITE_P(
    FineGrainedAccessControl, FineGrainedAccessControlTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<FineGrainedAccessControlTest::ParamType>&
           info) { return database_api::DatabaseDialect_Name(info.param); });

// Mirrors TestIntegration_QueryWithRoles of the Go client.
TEST_P(FineGrainedAccessControlTest, QueryWithRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_reader",
      "CREATE ROLE singers_unauthorized",
      "CREATE ROLE singers_reader_revoked",
      "CREATE ROLE dropped",
      "DROP ROLE dropped",
      Grant("SELECT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_reader"),
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers",
            "singers_unauthorized"),
      Grant("SELECT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_reader_revoked"),
      Revoke("SELECT(LastName) ON TABLE Singers", "singers_reader_revoked"),
  }));
  const std::string query = "SELECT SingerId, FirstName, LastName FROM Singers";

  EXPECT_THAT(Query(query), IsOkAndHoldsRows({{1, "Marc", "Richards"}}));
  EXPECT_THAT(QueryAs("singers_reader", query),
              IsOkAndHoldsRows({{1, "Marc", "Richards"}}));

  EXPECT_THAT(QueryAs("singers_unauthorized", query),
              LacksPrivileges("singers_unauthorized", "table", Name("Singers")));
  EXPECT_THAT(
      QueryAs("singers_reader_revoked", query),
      LacksPrivileges("singers_reader_revoked", "table", Name("Singers")));
  EXPECT_THAT(QueryAs("nonexistent", query), RoleNotFound("nonexistent"));
  EXPECT_THAT(QueryAs("dropped", query), RoleNotFound("dropped"));
}

TEST_P(FineGrainedAccessControlTest, QueryNeedsSelectOnEveryScannedColumn) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_reader",
      "CREATE ROLE table_reader",
      "CREATE ROLE singers_creator",
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers", "singers_reader"),
      Grant("SELECT ON TABLE Singers", "table_reader"),
      Grant("INSERT ON TABLE Singers", "singers_creator"),
  }));

  // Columns in the WHERE clause are read as well.
  EXPECT_THAT(QueryAs("singers_reader",
                      "SELECT FirstName FROM Singers WHERE SingerId = 1"),
              IsOkAndHoldsRows({{"Marc"}}));
  EXPECT_THAT(
      QueryAs("singers_reader",
              "SELECT FirstName FROM Singers WHERE LastName = 'Richards'"),
      LacksPrivileges("singers_reader", "table", Name("Singers")));

  // SELECT * reads every column, so it needs a table-level grant.
  EXPECT_THAT(QueryAs("singers_reader", "SELECT * FROM Singers"),
              LacksPrivileges("singers_reader", "table", Name("Singers")));
  EXPECT_THAT(
      QueryAs("table_reader", "SELECT * FROM Singers"),
      IsOkAndHoldsRows({{1, "Marc", "Richards", Null<Bytes>()}}));

  // COUNT(*) needs SELECT on at least one column.
  EXPECT_THAT(QueryAs("singers_reader", "SELECT COUNT(*) FROM Singers"),
              IsOkAndHoldsRows({{1}}));
  EXPECT_THAT(QueryAs("singers_creator", "SELECT COUNT(*) FROM Singers"),
              LacksPrivileges("singers_creator", "table", Name("Singers")));
}

// Mirrors TestIntegration_ReadWithRoles of the Go client.
TEST_P(FineGrainedAccessControlTest, ReadWithRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_reader",
      "CREATE ROLE singers_unauthorized",
      "CREATE ROLE singers_reader_revoked",
      "CREATE ROLE dropped",
      "DROP ROLE dropped",
      Grant("SELECT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_reader"),
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers",
            "singers_unauthorized"),
      Grant("SELECT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_reader_revoked"),
      Revoke("SELECT(LastName) ON TABLE Singers", "singers_reader_revoked"),
  }));

  EXPECT_THAT(ReadAll(Name("Singers"), SingerColumns()),
              IsOkAndHoldsRows({{1, "Marc", "Richards"}}));
  EXPECT_THAT(ReadAs("singers_reader", Name("Singers"), SingerColumns()),
              IsOkAndHoldsRows({{1, "Marc", "Richards"}}));

  EXPECT_THAT(ReadAs("singers_unauthorized", Name("Singers"), SingerColumns()),
              LacksPrivileges("singers_unauthorized", "table", Name("Singers")));
  EXPECT_THAT(
      ReadAs("singers_reader_revoked", Name("Singers"), SingerColumns()),
      LacksPrivileges("singers_reader_revoked", "table", Name("Singers")));
  EXPECT_THAT(ReadAs("nonexistent", Name("Singers"), SingerColumns()),
              RoleNotFound("nonexistent"));
  EXPECT_THAT(ReadAs("dropped", Name("Singers"), SingerColumns()),
              RoleNotFound("dropped"));

  // A read of the granted columns only succeeds.
  EXPECT_THAT(ReadAs("singers_unauthorized", Name("Singers"),
                     {Name("SingerId"), Name("FirstName")}),
              IsOkAndHoldsRows({{1, "Marc"}}));
}

// Mirrors TestIntegration_DMLWithRoles of the Go client.
TEST_P(FineGrainedAccessControlTest, DmlWithRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_updater",
      "CREATE ROLE singers_unauthorized",
      "CREATE ROLE singers_creator",
      "CREATE ROLE singers_deleter",
      Grant("SELECT(SingerId), UPDATE(FirstName, LastName) ON TABLE Singers",
            "singers_updater"),
      Grant("SELECT(SingerId), UPDATE(FirstName) ON TABLE Singers",
            "singers_unauthorized"),
      Grant("INSERT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_creator"),
      Grant("SELECT(SingerId), DELETE ON TABLE Singers", "singers_deleter"),
  }));
  const std::string update =
      "UPDATE Singers SET FirstName = 'Mark', LastName = 'Richards' "
      "WHERE SingerId = 1";

  GOOGLESQL_EXPECT_OK(CommitDml({update}));
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs("singers_updater", update));

  EXPECT_THAT(ExecuteDmlAs("singers_unauthorized", update),
              LacksPrivileges("singers_unauthorized", "table", Name("Singers")));
  EXPECT_THAT(ExecuteDmlAs("nonexistent", update),
              RoleNotFound("nonexistent"));

  GOOGLESQL_EXPECT_OK(CommitDml(
      {"INSERT INTO Singers (SingerId, FirstName, LastName) "
       "VALUES (2, 'Catalina', 'Smith')"}));
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs(
      "singers_creator",
      "INSERT INTO Singers (SingerId, FirstName, LastName) "
      "VALUES (3, 'Alice', 'Trentor')"));
  EXPECT_THAT(Query("SELECT SingerId, FirstName, LastName FROM Singers"),
              IsOkAndHoldsUnorderedRows({{1, "Mark", "Richards"},
                                         {2, "Catalina", "Smith"},
                                         {3, "Alice", "Trentor"}}));

  GOOGLESQL_EXPECT_OK(
      ExecuteDmlAs("singers_deleter", "DELETE FROM Singers WHERE TRUE"));
  EXPECT_THAT(Query("SELECT SingerId FROM Singers"), IsOkAndHoldsRows({}));
}

TEST_P(FineGrainedAccessControlTest, DmlNeedsSelectOnReadColumns) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_updater",
      "CREATE ROLE singers_creator",
      "CREATE ROLE singers_creator_reader",
      "CREATE ROLE singers_deleter",
      Grant("SELECT(SingerId), UPDATE(FirstName, LastName) ON TABLE Singers",
            "singers_updater"),
      Grant("INSERT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_creator"),
      Grant("SELECT(SingerId), INSERT(SingerId, FirstName, LastName) "
            "ON TABLE Singers",
            "singers_creator_reader"),
      Grant("SELECT(SingerId), DELETE ON TABLE Singers", "singers_deleter"),
  }));

  // UPDATE needs SELECT on the columns its WHERE clause and SET expressions
  // read.
  EXPECT_THAT(
      ExecuteDmlAs("singers_updater",
                   "UPDATE Singers SET FirstName = 'Mark' "
                   "WHERE LastName = 'Richards'"),
      LacksPrivileges("singers_updater", "table", Name("Singers")));
  EXPECT_THAT(
      ExecuteDmlAs("singers_updater",
                   "UPDATE Singers SET FirstName = LastName WHERE SingerId = 1"),
      LacksPrivileges("singers_updater", "table", Name("Singers")));

  // UPDATE does not imply INSERT or DELETE.
  EXPECT_THAT(ExecuteDmlAs("singers_updater",
                           "INSERT INTO Singers (SingerId, FirstName) "
                           "VALUES (4, 'Lea')"),
              LacksPrivileges("singers_updater", "table", Name("Singers")));
  EXPECT_THAT(
      ExecuteDmlAs("singers_updater", "DELETE FROM Singers WHERE SingerId = 1"),
      LacksPrivileges("singers_updater", "table", Name("Singers")));

  // DELETE needs SELECT on the columns of its WHERE clause.
  EXPECT_THAT(
      ExecuteDmlAs("singers_deleter",
                   "DELETE FROM Singers WHERE FirstName = 'Marc'"),
      LacksPrivileges("singers_deleter", "table", Name("Singers")));

  // Returned columns need SELECT.
  const std::string insert_returning = Ddl(
      "INSERT INTO Singers (SingerId, FirstName, LastName) "
      "VALUES (5, 'Lea', 'Martin') THEN RETURN SingerId",
      "INSERT INTO Singers (SingerId, FirstName, LastName) "
      "VALUES (5, 'Lea', 'Martin') RETURNING SingerId");
  EXPECT_THAT(ReadWriteQueryAs("singers_creator", insert_returning),
              LacksPrivileges("singers_creator", "table", Name("Singers")));
  EXPECT_THAT(ReadWriteQueryAs("singers_creator_reader", insert_returning),
              IsOkAndHoldsRows({{5}}));
}

// Mirrors TestIntegration_MutationWithRoles of the Go client.
TEST_P(FineGrainedAccessControlTest, MutationWithRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_updater",
      "CREATE ROLE singers_unauthorized",
      "CREATE ROLE singers_creator",
      "CREATE ROLE singers_deleter",
      Grant("SELECT(SingerId), UPDATE(SingerId, FirstName, LastName) "
            "ON TABLE Singers",
            "singers_updater"),
      Grant("SELECT(SingerId), UPDATE(SingerId, FirstName) ON TABLE Singers",
            "singers_unauthorized"),
      Grant("INSERT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_creator"),
      Grant("SELECT(SingerId), DELETE ON TABLE Singers", "singers_deleter"),
  }));
  const std::string singers = Name("Singers");

  GOOGLESQL_EXPECT_OK(
      Commit({MakeUpdate(singers, SingerColumns(), 1, "Mark", "Richards")}));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "singers_updater",
      {MakeUpdate(singers, SingerColumns(), 1, "Mark", "Richards")}));

  EXPECT_THAT(CommitAs("singers_unauthorized",
                       {MakeUpdate(singers, SingerColumns(), 1, "Mark",
                                   "Richards")}),
              LacksPrivileges("singers_unauthorized", "table", singers));
  EXPECT_THAT(CommitAs("nonexistent", {MakeUpdate(singers, SingerColumns(), 1,
                                                  "Mark", "Richards")}),
              RoleNotFound("nonexistent"));

  GOOGLESQL_EXPECT_OK(
      Commit({MakeInsert(singers, SingerColumns(), 2, "Catalina", "Smith")}));
  GOOGLESQL_EXPECT_OK(
      CommitAs("singers_creator",
               {MakeInsert(singers, SingerColumns(), 3, "Alice", "Trentor")}));

  GOOGLESQL_EXPECT_OK(Commit({MakeDelete(singers, Singleton(2))}));
  GOOGLESQL_EXPECT_OK(
      CommitAs("singers_deleter", {MakeDelete(singers, Singleton(1))}));
  EXPECT_THAT(Query("SELECT SingerId, FirstName, LastName FROM Singers"),
              IsOkAndHoldsRows({{3, "Alice", "Trentor"}}));
}

TEST_P(FineGrainedAccessControlTest, MutationTypesNeedMatchingPrivileges) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_creator",
      "CREATE ROLE singers_updater",
      "CREATE ROLE singers_upserter",
      "CREATE ROLE singers_replacer",
      Grant("INSERT(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_creator"),
      Grant("UPDATE(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_updater"),
      Grant("INSERT(SingerId, FirstName, LastName), "
            "UPDATE(SingerId, FirstName, LastName) ON TABLE Singers",
            "singers_upserter"),
      Grant("INSERT(SingerId, FirstName, LastName), DELETE ON TABLE Singers",
            "singers_replacer"),
  }));
  const std::string singers = Name("Singers");

  // InsertOrUpdate needs INSERT and UPDATE on the written columns.
  EXPECT_THAT(CommitAs("singers_creator",
                       {MakeInsertOrUpdate(singers, SingerColumns(), 1, "Mark",
                                           "Richards")}),
              LacksPrivileges("singers_creator", "table", singers));
  EXPECT_THAT(CommitAs("singers_updater",
                       {MakeInsertOrUpdate(singers, SingerColumns(), 1, "Mark",
                                           "Richards")}),
              LacksPrivileges("singers_updater", "table", singers));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "singers_upserter",
      {MakeInsertOrUpdate(singers, SingerColumns(), 1, "Mark", "Richards")}));

  // An update of an unlisted column needs UPDATE on it.
  EXPECT_THAT(CommitAs("singers_updater",
                       {MakeUpdate(singers,
                                   {Name("SingerId"), Name("SingerInfo")}, 1,
                                   Bytes("info"))}),
              LacksPrivileges("singers_updater", "table", singers));

  // Replace needs INSERT on the written columns and DELETE on the table.
  EXPECT_THAT(CommitAs("singers_creator",
                       {MakeReplace(singers, SingerColumns(), 1, "Marc",
                                    "Richards")}),
              LacksPrivileges("singers_creator", "table", singers));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "singers_replacer",
      {MakeReplace(singers, SingerColumns(), 1, "Marc", "Richards")}));

  // Delete needs DELETE on the table.
  EXPECT_THAT(CommitAs("singers_upserter", {MakeDelete(singers, Singleton(1))}),
              LacksPrivileges("singers_upserter", "table", singers));
  GOOGLESQL_EXPECT_OK(
      CommitAs("singers_replacer", {MakeDelete(singers, Singleton(1))}));

  // BatchWrite applies the same checks.
  GOOGLESQL_EXPECT_OK(BatchWriteAs(
      "singers_creator",
      {MakeInsert(singers, SingerColumns(), 2, "Catalina", "Smith")}));
  EXPECT_THAT(BatchWriteAs("singers_updater",
                           {MakeInsert(singers, SingerColumns(), 3, "Alice",
                                       "Trentor")}),
              LacksPrivileges("singers_updater", "table", singers));
  EXPECT_THAT(Query("SELECT SingerId, FirstName, LastName FROM Singers"),
              IsOkAndHoldsRows({{2, "Catalina", "Smith"}}));
}

// Mirrors TestIntegration_ListDatabaseRoles of the Go client.
TEST_P(FineGrainedAccessControlTest, ListDatabaseRolesIncludesSystemRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({"CREATE ROLE a", "CREATE ROLE z"}));
  EXPECT_THAT(ListDatabaseRoleNames(),
              IsOkAndHolds(ElementsAre("a", "public", "spanner_info_reader",
                                       "spanner_sys_reader", "z")));
}

TEST_P(FineGrainedAccessControlTest, SessionsEchoCreatorRole) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({"CREATE ROLE singers_reader"}));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::Session session,
                                 CreateSession("singers_reader"));
  EXPECT_EQ(session.creator_role(), "singers_reader");
  EXPECT_THAT(GetSession(session.name()),
              IsOkAndHolds(testing::Property(&spanner_api::Session::creator_role,
                                             "singers_reader")));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      spanner_api::Session multiplexed,
      CreateSession("singers_reader", /*multiplexed=*/true));
  EXPECT_EQ(multiplexed.creator_role(), "singers_reader");

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<spanner_api::Session> sessions,
                                 BatchCreateSessions("singers_reader", 2));
  ASSERT_THAT(sessions, SizeIs(2));
  for (const spanner_api::Session& batch_session : sessions) {
    EXPECT_EQ(batch_session.creator_role(), "singers_reader");
  }

  // Sessions may use the system roles.
  for (const std::string role :
       {"public", "spanner_info_reader", "spanner_sys_reader"}) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::Session system_session,
                                   CreateSession(role));
    EXPECT_EQ(system_session.creator_role(), role);
  }

  // A session without a role has no creator role.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::Session no_role,
                                 CreateSession(""));
  EXPECT_THAT(no_role.creator_role(), IsEmpty());
}

TEST_P(FineGrainedAccessControlTest, SessionsCannotUseUnknownRole) {
  GOOGLESQL_ASSERT_OK(
      UpdateSchema({"CREATE ROLE dropped", "DROP ROLE dropped"}));
  for (const std::string role : {"nonexistent", "dropped"}) {
    EXPECT_THAT(CreateSession(role), RoleNotFound(role));
    EXPECT_THAT(CreateSession(role, /*multiplexed=*/true), RoleNotFound(role));
    EXPECT_THAT(BatchCreateSessions(role, 2), RoleNotFound(role));
  }
}

TEST_P(FineGrainedAccessControlTest, DroppedRoleFailsExistingSession) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_reader",
      Grant("SELECT ON TABLE Singers", "singers_reader"),
  }));
  const std::string query = "SELECT SingerId FROM Singers";
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(spanner_api::Session session,
                                 CreateSession("singers_reader"));
  GOOGLESQL_EXPECT_OK(ExecuteSqlInSession(session.name(), query));

  // A role cannot be dropped while it holds privileges.
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Revoke("SELECT ON TABLE Singers", "singers_reader"),
      "DROP ROLE singers_reader",
  }));
  EXPECT_THAT(ExecuteSqlInSession(session.name(), query),
              RoleNotFound("singers_reader"));
}

TEST_P(FineGrainedAccessControlTest, PublicPrivilegesApplyToEveryRole) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({"CREATE ROLE no_grants"}));
  const std::string query = "SELECT SingerId FROM Singers";
  EXPECT_THAT(QueryAs("public", query),
              LacksPrivileges("public", "table", Name("Singers")));
  EXPECT_THAT(QueryAs("no_grants", query),
              LacksPrivileges("no_grants", "table", Name("Singers")));

  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Grant("SELECT(SingerId) ON TABLE Singers", "public"),
      "CREATE ROLE created_later",
  }));
  EXPECT_THAT(QueryAs("public", query), IsOkAndHoldsRows({{1}}));
  EXPECT_THAT(QueryAs("no_grants", query), IsOkAndHoldsRows({{1}}));
  EXPECT_THAT(QueryAs("created_later", query), IsOkAndHoldsRows({{1}}));
  EXPECT_THAT(QueryAs("no_grants", "SELECT FirstName FROM Singers"),
              LacksPrivileges("no_grants", "table", Name("Singers")));
}

TEST_P(FineGrainedAccessControlTest, RolesInheritPrivilegesTransitively) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE parent",
      "CREATE ROLE child",
      "CREATE ROLE grandchild",
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers", "parent"),
      Grant("INSERT(SingerId, FirstName, LastName) ON TABLE Singers", "child"),
      GrantRole("parent", "child"),
      GrantRole("child", "grandchild"),
  }));
  const std::string query = "SELECT SingerId, FirstName FROM Singers";
  const std::string singers = Name("Singers");

  EXPECT_THAT(QueryAs("grandchild", query), IsOkAndHoldsRows({{1, "Marc"}}));
  GOOGLESQL_EXPECT_OK(
      CommitAs("grandchild",
               {MakeInsert(singers, SingerColumns(), 2, "Catalina", "Smith")}));

  // Members inherit from the roles they belong to, not the other way round.
  EXPECT_THAT(CommitAs("parent", {MakeInsert(singers, SingerColumns(), 3,
                                             "Alice", "Trentor")}),
              LacksPrivileges("parent", "table", singers));

  GOOGLESQL_ASSERT_OK(UpdateSchema({RevokeRole("child", "grandchild")}));
  EXPECT_THAT(QueryAs("grandchild", query),
              LacksPrivileges("grandchild", "table", singers));
  EXPECT_THAT(QueryAs("child", query),
              IsOkAndHoldsUnorderedRows({{1, "Marc"}, {2, "Catalina"}}));
}

TEST_P(FineGrainedAccessControlTest, RevokeRemovesPrivileges) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_admin",
      Grant("SELECT, INSERT, UPDATE, DELETE ON TABLE Singers", "singers_admin"),
  }));
  const std::string query = "SELECT SingerId FROM Singers";
  const std::string singers = Name("Singers");
  EXPECT_THAT(QueryAs("singers_admin", query), IsOkAndHoldsRows({{1}}));

  GOOGLESQL_ASSERT_OK(
      UpdateSchema({Revoke("SELECT ON TABLE Singers", "singers_admin")}));
  EXPECT_THAT(QueryAs("singers_admin", query),
              LacksPrivileges("singers_admin", "table", singers));
  GOOGLESQL_EXPECT_OK(
      CommitAs("singers_admin",
               {MakeInsert(singers, SingerColumns(), 2, "Catalina", "Smith")}));

  GOOGLESQL_ASSERT_OK(
      UpdateSchema({Revoke("INSERT, DELETE ON TABLE Singers", "singers_admin")}));
  EXPECT_THAT(CommitAs("singers_admin", {MakeInsert(singers, SingerColumns(),
                                                    3, "Alice", "Trentor")}),
              LacksPrivileges("singers_admin", "table", singers));
  EXPECT_THAT(CommitAs("singers_admin", {MakeDelete(singers, Singleton(2))}),
              LacksPrivileges("singers_admin", "table", singers));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "singers_admin",
      {MakeUpdate(singers, SingerColumns(), 2, "Catalina", "Jones")}));
}

TEST_P(FineGrainedAccessControlTest, DefinerAndInvokerViews) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Ddl("CREATE VIEW SingerNamesInvoker SQL SECURITY INVOKER AS "
          "SELECT Singers.SingerId, Singers.FirstName FROM Singers",
          "CREATE VIEW SingerNamesInvoker SQL SECURITY INVOKER AS "
          "SELECT SingerId, FirstName FROM Singers"),
      Ddl("CREATE VIEW SingerNamesDefiner SQL SECURITY DEFINER AS "
          "SELECT Singers.SingerId, Singers.FirstName FROM Singers",
          "CREATE VIEW SingerNamesDefiner SQL SECURITY DEFINER AS "
          "SELECT SingerId, FirstName FROM Singers"),
      "CREATE ROLE view_reader",
      "CREATE ROLE invoker_reader",
      "CREATE ROLE table_reader",
      Grant(Ddl("SELECT ON VIEW SingerNamesInvoker, SingerNamesDefiner",
                "SELECT ON TABLE SingerNamesInvoker, SingerNamesDefiner"),
            "view_reader"),
      Grant(Ddl("SELECT ON VIEW SingerNamesInvoker",
                "SELECT ON TABLE SingerNamesInvoker"),
            "invoker_reader"),
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers", "invoker_reader"),
      Grant("SELECT ON TABLE Singers", "table_reader"),
  }));
  const std::string definer_query =
      "SELECT SingerId, FirstName FROM SingerNamesDefiner";
  const std::string invoker_query =
      "SELECT SingerId, FirstName FROM SingerNamesInvoker";

  EXPECT_THAT(Query(definer_query), IsOkAndHoldsRows({{1, "Marc"}}));
  EXPECT_THAT(Query(invoker_query), IsOkAndHoldsRows({{1, "Marc"}}));

  // A DEFINER view needs SELECT on the view only.
  EXPECT_THAT(QueryAs("view_reader", definer_query),
              IsOkAndHoldsRows({{1, "Marc"}}));

  // An INVOKER view also needs the privileges its query uses.
  EXPECT_THAT(QueryAs("view_reader", invoker_query),
              LacksPrivileges("view_reader", "table", Name("Singers")));
  EXPECT_THAT(QueryAs("invoker_reader", invoker_query),
              IsOkAndHoldsRows({{1, "Marc"}}));

  // Privileges on the underlying table do not grant access to a view.
  EXPECT_THAT(QueryAs("table_reader", definer_query),
              LacksPrivileges("table_reader", "view",
                              Name("SingerNamesDefiner")));
  EXPECT_THAT(QueryAs("table_reader", invoker_query),
              LacksPrivileges("table_reader", "view",
                              Name("SingerNamesInvoker")));
}

TEST_P(FineGrainedAccessControlTest, PlanChecksInvokerViewQueries) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Ddl("CREATE VIEW SingerNamesInvoker SQL SECURITY INVOKER AS "
          "SELECT Singers.SingerId, Singers.FirstName FROM Singers",
          "CREATE VIEW SingerNamesInvoker SQL SECURITY INVOKER AS "
          "SELECT SingerId, FirstName FROM Singers"),
      Ddl("CREATE VIEW SingerNamesDefiner SQL SECURITY DEFINER AS "
          "SELECT Singers.SingerId, Singers.FirstName FROM Singers",
          "CREATE VIEW SingerNamesDefiner SQL SECURITY DEFINER AS "
          "SELECT SingerId, FirstName FROM Singers"),
      // An INVOKER view over an INVOKER view.
      Ddl("CREATE VIEW NestedInvoker SQL SECURITY INVOKER AS "
          "SELECT SingerNamesInvoker.SingerId FROM SingerNamesInvoker",
          "CREATE VIEW NestedInvoker SQL SECURITY INVOKER AS "
          "SELECT SingerId FROM SingerNamesInvoker"),
      "CREATE ROLE view_reader",
      "CREATE ROLE invoker_reader",
      Grant(Ddl("SELECT ON VIEW SingerNamesInvoker, SingerNamesDefiner, "
                "NestedInvoker",
                "SELECT ON TABLE SingerNamesInvoker, SingerNamesDefiner, "
                "NestedInvoker"),
            "view_reader"),
      Grant(Ddl("SELECT ON VIEW SingerNamesInvoker, NestedInvoker",
                "SELECT ON TABLE SingerNamesInvoker, NestedInvoker"),
            "invoker_reader"),
      Grant("SELECT(SingerId, FirstName) ON TABLE Singers", "invoker_reader"),
  }));

  // PLAN evaluates no view, but still checks the objects that the query of
  // an INVOKER view reads.
  EXPECT_THAT(PlanQueryAs("view_reader", "SELECT SingerId FROM "
                                         "SingerNamesInvoker"),
              LacksPrivileges("view_reader", "table", Name("Singers")));
  EXPECT_THAT(PlanQueryAs("view_reader", "SELECT SingerId FROM NestedInvoker"),
              LacksPrivileges("view_reader", "table", Name("Singers")));
  GOOGLESQL_EXPECT_OK(
      PlanQueryAs("view_reader", "SELECT SingerId FROM SingerNamesDefiner"));
  GOOGLESQL_EXPECT_OK(PlanQueryAs("invoker_reader",
                                  "SELECT SingerId FROM SingerNamesInvoker"));
  GOOGLESQL_EXPECT_OK(
      PlanQueryAs("invoker_reader", "SELECT SingerId FROM NestedInvoker"));
  EXPECT_THAT(QueryAs("invoker_reader", "SELECT SingerId FROM NestedInvoker"),
              IsOkAndHoldsRows({{1}}));
}

TEST_P(FineGrainedAccessControlTest, DefaultValuesNeedSequencePrivileges) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Ddl("CREATE SEQUENCE AccountIds "
          "OPTIONS (sequence_kind = 'bit_reversed_positive')",
          "CREATE SEQUENCE AccountIds BIT_REVERSED_POSITIVE"),
      Ddl("CREATE TABLE Accounts ("
          "  AccountId INT64 NOT NULL DEFAULT "
          "    (GET_NEXT_SEQUENCE_VALUE(SEQUENCE AccountIds)),"
          "  Name STRING(MAX)"
          ") PRIMARY KEY (AccountId)",
          "CREATE TABLE Accounts ("
          "  AccountId bigint NOT NULL DEFAULT nextval('accountids'),"
          "  Name varchar,"
          "  PRIMARY KEY (AccountId))"),
      "CREATE ROLE writer",
      "CREATE ROLE sequence_writer",
      Grant("SELECT, INSERT, UPDATE ON TABLE Accounts", "writer"),
      Grant("SELECT, INSERT, UPDATE ON TABLE Accounts", "sequence_writer"),
      Grant("UPDATE ON SEQUENCE AccountIds", "sequence_writer"),
  }));
  const std::string sequence = Name("AccountIds");
  const std::string insert_default =
      "INSERT INTO Accounts (Name) VALUES ('generated')";
  const std::string insert_default_keyword =
      "INSERT INTO Accounts (AccountId, Name) VALUES (DEFAULT, 'keyword')";
  const std::string insert_explicit =
      "INSERT INTO Accounts (AccountId, Name) VALUES (7, 'explicit')";

  // A DML statement that computes a default value calls the sequence.
  EXPECT_THAT(ExecuteDmlAs("writer", insert_default),
              LacksPrivileges("writer", "sequence", sequence));
  EXPECT_THAT(ExecuteDmlAs("writer", insert_default_keyword),
              LacksPrivileges("writer", "sequence", sequence));
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs("writer", insert_explicit));
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs("sequence_writer", insert_default));
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs("sequence_writer", insert_default_keyword));

  // So does an insert mutation that omits the column.
  const std::string accounts = Name("Accounts");
  EXPECT_THAT(CommitAs("writer", {MakeInsert(accounts, {Name("Name")},
                                             "mutation")}),
              LacksPrivileges("writer", "sequence", sequence));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "writer",
      {MakeInsert(accounts, {Name("AccountId"), Name("Name")}, 8, "mutation")}));
  GOOGLESQL_EXPECT_OK(CommitAs(
      "sequence_writer", {MakeInsert(accounts, {Name("Name")}, "mutation")}));

  // Updating other columns does not compute the default value.
  GOOGLESQL_EXPECT_OK(ExecuteDmlAs(
      "writer", "UPDATE Accounts SET Name = 'renamed' WHERE AccountId = 7"));
}

TEST_P(FineGrainedAccessControlTest, ChangeStreamNeedsSelectAndExecute) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE CHANGE STREAM SingersStream FOR Singers",
      "CREATE ROLE stream_reader",
      "CREATE ROLE stream_selector",
      "CREATE ROLE stream_executor",
      "CREATE ROLE table_reader",
      Grant("SELECT ON CHANGE STREAM SingersStream", "stream_reader"),
      Grant(Ddl("EXECUTE ON TABLE FUNCTION READ_SingersStream",
                "EXECUTE ON FUNCTION spanner.read_json_SingersStream"),
            "stream_reader"),
      Grant("SELECT ON CHANGE STREAM SingersStream", "stream_selector"),
      Grant(Ddl("EXECUTE ON TABLE FUNCTION READ_SingersStream",
                "EXECUTE ON FUNCTION spanner.read_json_SingersStream"),
            "stream_executor"),
      Grant("SELECT ON TABLE Singers", "table_reader"),
  }));
  const std::string stream = Name("SingersStream");
  const std::string read_function =
      Ddl("READ_SingersStream", "read_json_singersstream");

  EXPECT_THAT(ReadChangeStreamAs("", stream), IsOkAndHolds(Not(IsEmpty())));
  EXPECT_THAT(ReadChangeStreamAs("stream_reader", stream),
              IsOkAndHolds(Not(IsEmpty())));

  EXPECT_THAT(ReadChangeStreamAs("stream_selector", stream),
              LacksPrivileges("stream_selector", "table function",
                              read_function));
  EXPECT_THAT(
      ReadChangeStreamAs("stream_executor", stream),
      LacksPrivileges("stream_executor", "change stream", stream));
  // SELECT on the tracked table does not grant access to the change stream.
  EXPECT_THAT(ReadChangeStreamAs("table_reader", stream),
              LacksPrivileges("table_reader", "change stream", stream));
}

TEST_P(FineGrainedAccessControlTest, SequenceFunctionsNeedPrivileges) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Ddl("CREATE SEQUENCE SingerIdSequence "
          "OPTIONS (sequence_kind = 'bit_reversed_positive')",
          "CREATE SEQUENCE SingerIdSequence BIT_REVERSED_POSITIVE"),
      "CREATE ROLE sequence_selector",
      "CREATE ROLE sequence_updater",
      "CREATE ROLE no_grants",
      Grant("SELECT ON SEQUENCE SingerIdSequence", "sequence_selector"),
      Grant("UPDATE ON SEQUENCE SingerIdSequence", "sequence_updater"),
  }));
  const std::string sequence = Name("SingerIdSequence");
  const std::string next_value =
      Ddl("SELECT GET_NEXT_SEQUENCE_VALUE(SEQUENCE SingerIdSequence) > 0",
          "SELECT nextval('singeridsequence') > 0");
  const std::string internal_state =
      Ddl("SELECT GET_INTERNAL_SEQUENCE_STATE(SEQUENCE SingerIdSequence) "
          "IS NULL",
          "SELECT spanner.get_internal_sequence_state('singeridsequence') "
          "IS NULL");

  // GET_NEXT_SEQUENCE_VALUE needs SELECT or UPDATE on the sequence.
  EXPECT_THAT(ReadWriteQueryAs("sequence_selector", next_value),
              IsOkAndHoldsRows({{true}}));
  EXPECT_THAT(ReadWriteQueryAs("sequence_updater", next_value),
              IsOkAndHoldsRows({{true}}));
  EXPECT_THAT(ReadWriteQueryAs("no_grants", next_value),
              LacksPrivileges("no_grants", "sequence", sequence));

  // GET_INTERNAL_SEQUENCE_STATE needs SELECT.
  EXPECT_THAT(QueryAs("sequence_selector", internal_state),
              IsOkAndHoldsRows({{false}}));
  EXPECT_THAT(QueryAs("sequence_updater", internal_state),
              LacksPrivileges("sequence_updater", "sequence", sequence));
  EXPECT_THAT(QueryAs("no_grants", internal_state),
              LacksPrivileges("no_grants", "sequence", sequence));
}

TEST_P(FineGrainedAccessControlTest, NamedSchemaNeedsUsage) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE SCHEMA sch",
      Ddl("CREATE TABLE sch.Albums (AlbumId INT64 NOT NULL, "
          "Title STRING(MAX)) PRIMARY KEY (AlbumId)",
          "CREATE TABLE sch.Albums (AlbumId bigint NOT NULL, "
          "Title varchar, PRIMARY KEY (AlbumId))"),
      "CREATE ROLE albums_reader",
      "CREATE ROLE albums_no_usage",
      "CREATE ROLE usage_only",
      Grant("SELECT ON TABLE sch.Albums", "albums_reader"),
      Grant("USAGE ON SCHEMA sch", "albums_reader"),
      Grant("SELECT ON TABLE sch.Albums", "albums_no_usage"),
      Grant("USAGE ON SCHEMA sch", "usage_only"),
  }));
  const std::string albums = Name("sch.Albums");
  const std::vector<std::string> columns = {Name("AlbumId"), Name("Title")};
  GOOGLESQL_ASSERT_OK(Insert(albums, columns, {1, "Total Junk"}));
  const std::string query = "SELECT AlbumId, Title FROM sch.Albums";

  EXPECT_THAT(QueryAs("albums_reader", query),
              IsOkAndHoldsRows({{1, "Total Junk"}}));
  EXPECT_THAT(ReadAs("albums_reader", albums, columns),
              IsOkAndHoldsRows({{1, "Total Junk"}}));

  EXPECT_THAT(QueryAs("albums_no_usage", query),
              LacksPrivileges("albums_no_usage", "schema", "sch"));
  EXPECT_THAT(ReadAs("albums_no_usage", albums, columns),
              LacksPrivileges("albums_no_usage", "schema", "sch"));
  EXPECT_THAT(CommitAs("albums_no_usage",
                       {MakeInsert(albums, columns, 2, "Other")}),
              LacksPrivileges("albums_no_usage", "schema", "sch"));

  // USAGE on the schema does not grant access to its objects.
  EXPECT_THAT(QueryAs("usage_only", query),
              LacksPrivileges("usage_only", "table", albums));
}

TEST_P(FineGrainedAccessControlTest, DefaultSchemaUsageCanBeRevokedFromPublic) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE singers_reader",
      Grant("SELECT ON TABLE Singers", "singers_reader"),
  }));
  const std::string query = "SELECT SingerId FROM Singers";
  EXPECT_THAT(QueryAs("singers_reader", query), IsOkAndHoldsRows({{1}}));

  const std::string default_schema = Ddl("DEFAULT", "public");
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      {Revoke(absl::StrCat("USAGE ON SCHEMA ", default_schema), "public")}));
  EXPECT_THAT(QueryAs("singers_reader", query),
              LacksPrivileges("singers_reader", "schema", default_schema));
  EXPECT_THAT(ReadAs("singers_reader", Name("Singers"), {Name("SingerId")}),
              LacksPrivileges("singers_reader", "schema", default_schema));

  GOOGLESQL_ASSERT_OK(UpdateSchema({Grant(
      absl::StrCat("USAGE ON SCHEMA ", default_schema), "singers_reader")}));
  EXPECT_THAT(QueryAs("singers_reader", query), IsOkAndHoldsRows({{1}}));
}

TEST_P(FineGrainedAccessControlTest, SpannerSysNeedsSpannerSysReader) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE sys_reader",
      "CREATE ROLE no_grants",
      GrantRole("spanner_sys_reader", "sys_reader"),
  }));
  const std::string query =
      "SELECT version FROM SPANNER_SYS.SUPPORTED_OPTIMIZER_VERSIONS";

  EXPECT_THAT(QueryAs("no_grants", query),
              LacksPrivileges("no_grants", "table",
                              "SPANNER_SYS.SUPPORTED_OPTIMIZER_VERSIONS"));
  EXPECT_THAT(QueryAs("sys_reader", query), IsOkAndHolds(Not(IsEmpty())));
  EXPECT_THAT(QueryAs("spanner_sys_reader", query),
              IsOkAndHolds(Not(IsEmpty())));
}

TEST_P(FineGrainedAccessControlTest, SystemCatalogsNeedNoPrivileges) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({"CREATE ROLE no_grants"}));
  GOOGLESQL_EXPECT_OK(QueryAs(
      "no_grants", Ddl("SELECT COUNT(*) FROM INFORMATION_SCHEMA.SCHEMATA",
                       "SELECT COUNT(*) FROM information_schema.schemata")));
  if (is_postgresql()) {
    GOOGLESQL_EXPECT_OK(
        QueryAs("no_grants", "SELECT COUNT(*) FROM pg_catalog.pg_namespace"));
  }
}

TEST_P(FineGrainedAccessControlTest, InformationSchemaTablesListsGrantedTables) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      Ddl("CREATE TABLE Albums (AlbumId INT64 NOT NULL) PRIMARY KEY (AlbumId)",
          "CREATE TABLE Albums (AlbumId bigint NOT NULL, "
          "PRIMARY KEY (AlbumId))"),
      "CREATE ROLE singers_reader",
      "CREATE ROLE info_reader",
      Grant("SELECT(SingerId) ON TABLE Singers", "singers_reader"),
      GrantRole("spanner_info_reader", "info_reader"),
  }));
  const std::string query =
      Ddl("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
          "WHERE TABLE_SCHEMA = '' AND TABLE_TYPE = 'BASE TABLE'",
          "SELECT table_name FROM information_schema.tables "
          "WHERE table_schema = 'public' AND table_type = 'BASE TABLE'");

  // A privilege on some columns of a table makes the table visible.
  EXPECT_THAT(QueryStringsAs("singers_reader", query),
              IsOkAndHolds(UnorderedElementsAre(Name("Singers"))));

  // Sessions without a role and members of spanner_info_reader see all rows.
  for (const std::string role : {"", "info_reader", "spanner_info_reader"}) {
    EXPECT_THAT(QueryStringsAs(role, query),
                IsOkAndHolds(
                    UnorderedElementsAre(Name("Albums"), Name("Singers"))))
        << "role: " << role;
  }
}

TEST_P(FineGrainedAccessControlTest, PgCatalogListsVisibleObjects) {
  if (!is_postgresql()) {
    GTEST_SKIP() << "pg_catalog is only available in PostgreSQL databases";
  }
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE TABLE albums (albumid bigint NOT NULL PRIMARY KEY, "
      "title varchar)",
      "CREATE INDEX albums_by_title ON albums (title)",
      "CREATE INDEX singers_by_last_name ON singers (lastname)",
      "CREATE VIEW singer_ids SQL SECURITY INVOKER AS "
      "SELECT singerid FROM singers",
      "CREATE SEQUENCE album_ids BIT_REVERSED_POSITIVE",
      "CREATE ROLE singers_reader",
      "CREATE ROLE info_reader",
      Grant("SELECT(singerid, firstname) ON TABLE singers", "singers_reader"),
      Grant("SELECT ON TABLE singer_ids", "singers_reader"),
      GrantRole("spanner_info_reader", "info_reader"),
  }));
  const std::string tables =
      "SELECT tablename FROM pg_catalog.pg_tables WHERE schemaname = 'public'";
  const std::string relations =
      "SELECT relname FROM pg_catalog.pg_class c "
      "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
      "WHERE n.nspname = 'public'";
  const std::string columns =
      "SELECT a.attname FROM pg_catalog.pg_attribute a "
      "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
      "WHERE c.relname = 'singers'";
  const std::string indexes =
      "SELECT indexname FROM pg_catalog.pg_indexes WHERE schemaname = 'public'";
  const std::string views =
      "SELECT viewname FROM pg_catalog.pg_views WHERE schemaname = 'public'";
  const std::string sequences =
      "SELECT sequencename FROM pg_catalog.pg_sequences "
      "WHERE schemaname = 'public'";

  // The role sees the objects it holds privileges on, the columns it may
  // read and the indexes whose columns it may all read.
  EXPECT_THAT(QueryStringsAs("singers_reader", tables),
              IsOkAndHolds(UnorderedElementsAre("singers")));
  EXPECT_THAT(QueryStringsAs("singers_reader", relations),
              IsOkAndHolds(UnorderedElementsAre("singers", "PK_singers",
                                                "singer_ids")));
  EXPECT_THAT(QueryStringsAs("singers_reader", columns),
              IsOkAndHolds(UnorderedElementsAre("singerid", "firstname")));
  EXPECT_THAT(QueryStringsAs("singers_reader", indexes),
              IsOkAndHolds(UnorderedElementsAre("PK_singers")));
  EXPECT_THAT(QueryStringsAs("singers_reader", views),
              IsOkAndHolds(UnorderedElementsAre("singer_ids")));
  EXPECT_THAT(QueryStringsAs("singers_reader", sequences),
              IsOkAndHolds(IsEmpty()));

  // Sessions without a role and members of spanner_info_reader see all rows.
  for (const std::string role : {"", "info_reader", "spanner_info_reader"}) {
    EXPECT_THAT(QueryStringsAs(role, tables),
                IsOkAndHolds(UnorderedElementsAre("albums", "singers")))
        << "role: " << role;
    EXPECT_THAT(QueryStringsAs(role, indexes),
                IsOkAndHolds(UnorderedElementsAre(
                    "PK_albums", "PK_singers", "albums_by_title",
                    "singers_by_last_name")))
        << "role: " << role;
    EXPECT_THAT(QueryStringsAs(role, sequences),
                IsOkAndHolds(UnorderedElementsAre("album_ids")))
        << "role: " << role;
  }
}

TEST_P(FineGrainedAccessControlTest, InformationSchemaRolesListsEffectiveRoles) {
  GOOGLESQL_ASSERT_OK(UpdateSchema({
      "CREATE ROLE parent",
      "CREATE ROLE child",
      "CREATE ROLE other",
      "CREATE ROLE info_reader",
      GrantRole("parent", "child"),
      GrantRole("spanner_info_reader", "info_reader"),
  }));
  const std::string query =
      Ddl("SELECT ROLE_NAME FROM INFORMATION_SCHEMA.ROLES",
          "SELECT role_name FROM information_schema.enabled_roles");
  // PostgreSQL's enabled_roles does not list public.
  std::vector<std::string> child_roles = {"child", "parent"};
  std::vector<std::string> all_roles = {"child",
                                        "info_reader",
                                        "other",
                                        "parent",
                                        "spanner_info_reader",
                                        "spanner_sys_reader"};
  if (!is_postgresql()) {
    child_roles.push_back("public");
    all_roles.push_back("public");
  }

  // A restricted role sees itself and the roles it inherits from.
  EXPECT_THAT(QueryStringsAs("child", query),
              IsOkAndHolds(UnorderedElementsAreArray(child_roles)));
  EXPECT_THAT(QueryStringsAs("info_reader", query),
              IsOkAndHolds(UnorderedElementsAreArray(all_roles)));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
