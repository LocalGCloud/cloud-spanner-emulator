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
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "backend/schema/catalog/grants.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/printer/print_ddl.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/schema/updater/schema_updater_tests/base.h"
#include "common/errors.h"
#include "common/limits.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace test {

namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

class GrantsTest : public SchemaUpdaterTest {
 protected:
  bool IsPostgreSQL() const {
    return GetParam() == database_api::DatabaseDialect::POSTGRESQL;
  }

  // Returns the statement for the dialect under test.
  std::string Ddl(absl::string_view googlesql, absl::string_view postgresql) {
    return std::string(IsPostgreSQL() ? postgresql : googlesql);
  }

  absl::StatusOr<std::unique_ptr<const Schema>> Create(
      const std::vector<std::string>& statements) {
    return CreateSchema(statements, "", GetParam(),
                        /*use_gsql_to_pg_translation=*/false);
  }

  absl::StatusOr<std::unique_ptr<const Schema>> Update(
      const Schema* schema, const std::vector<std::string>& statements) {
    return UpdateSchema(schema, statements, "", GetParam(),
                        /*use_gsql_to_pg_translation=*/false);
  }

  // Applies `statements` as committed DDL that is replayed at startup.
  absl::StatusOr<std::unique_ptr<const Schema>> Replay(
      const std::vector<std::string>& statements) {
    SchemaUpdater updater;
    SchemaChangeContext context{.type_factory = &type_factory_,
                                .table_id_generator = &table_id_generator_,
                                .column_id_generator = &column_id_generator_,
                                .storage = storage_.get(),
                                .pg_oid_assigner = pg_oid_assigner_.get()};
    return updater.ValidateSchemaFromDDL(
        SchemaChangeOperation{.statements = statements,
                              .database_dialect = GetParam(),
                              .replaying_committed_ddl = true},
        context, /*existing_schema=*/nullptr);
  }

  std::vector<std::string> BaseSchema() {
    return {
        Ddl("CREATE TABLE T (K INT64, A STRING(MAX), B STRING(MAX), "
            "G INT64 AS (K + 1) STORED) PRIMARY KEY (K)",
            "CREATE TABLE t (k bigint PRIMARY KEY, a varchar, b varchar, "
            "g bigint GENERATED ALWAYS AS (k + 1) STORED)"),
        Ddl("CREATE VIEW V SQL SECURITY INVOKER AS SELECT T.K FROM T",
            "CREATE VIEW v SQL SECURITY INVOKER AS SELECT k FROM t"),
        "CREATE ROLE parent",
        "CREATE ROLE child",
    };
  }

  // The GRANT and REVOKE statements printed for `schema`.
  std::vector<std::string> PrintedGrants(const Schema* schema) {
    std::vector<std::string> grants;
    const std::vector<std::string> statements =
        PrintDDLStatements(schema).value();
    for (const std::string& statement : statements) {
      if (absl::StartsWith(statement, "GRANT") ||
          absl::StartsWith(statement, "REVOKE")) {
        grants.push_back(statement);
      }
    }
    return grants;
  }
};

INSTANTIATE_TEST_SUITE_P(
    GrantsPerDialectTests, GrantsTest,
    testing::Values(database_api::DatabaseDialect::GOOGLE_STANDARD_SQL,
                    database_api::DatabaseDialect::POSTGRESQL),
    [](const testing::TestParamInfo<GrantsTest::ParamType>& info) {
      return database_api::DatabaseDialect_Name(info.param);
    });

TEST_P(GrantsTest, GrantsAreStoredAndPrinted) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT SELECT(K, A), INSERT ON TABLE T TO ROLE child",
                           "GRANT SELECT(k, a), INSERT ON t TO child"));
  statements.push_back(
      Ddl("GRANT SELECT ON VIEW V TO ROLE parent, public",
          "GRANT SELECT ON TABLE v TO parent, public"));
  statements.push_back(Ddl("GRANT ROLE parent, spanner_info_reader TO ROLE child",
                           "GRANT parent, spanner_info_reader TO child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));

  const Grants* grants = schema->grants();
  ASSERT_NE(grants, nullptr);
  EXPECT_EQ(grants->privileges().size(), 5);
  EXPECT_EQ(grants->memberships().size(), 2);

  if (IsPostgreSQL()) {
    EXPECT_THAT(
        PrintedGrants(schema.get()),
        ElementsAre("GRANT \"parent\" TO \"child\"",
                    "GRANT \"spanner_info_reader\" TO \"child\"",
                    "GRANT SELECT(\"k\", \"a\"), INSERT ON TABLE \"t\" TO "
                    "\"child\"",
                    "GRANT SELECT ON TABLE \"v\" TO \"parent\"",
                    "GRANT SELECT ON TABLE \"v\" TO public"));
  } else {
    EXPECT_THAT(PrintedGrants(schema.get()),
                ElementsAre("GRANT ROLE parent TO ROLE child",
                            "GRANT ROLE spanner_info_reader TO ROLE child",
                            "GRANT SELECT(K, A), INSERT ON TABLE T TO ROLE child",
                            "GRANT SELECT ON VIEW V TO ROLE parent",
                            "GRANT SELECT ON VIEW V TO ROLE public"));
  }
}

TEST_P(GrantsTest, PrintedGrantsRecreateTheSchema) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back("CREATE SCHEMA sch");
  statements.push_back(Ddl("CREATE TABLE sch.U (K INT64) PRIMARY KEY (K)",
                           "CREATE TABLE sch.u (k bigint PRIMARY KEY)"));
  statements.push_back(Ddl("CREATE SEQUENCE S OPTIONS "
                           "(sequence_kind = 'bit_reversed_positive')",
                           "CREATE SEQUENCE s BIT_REVERSED_POSITIVE"));
  statements.push_back(Ddl("CREATE CHANGE STREAM CS FOR T",
                           "CREATE CHANGE STREAM cs FOR t"));
  statements.push_back(Ddl("GRANT UPDATE(A), DELETE ON TABLE T TO ROLE child",
                           "GRANT UPDATE(a), DELETE ON t TO child"));
  statements.push_back(Ddl("GRANT SELECT ON TABLE sch.U TO ROLE child",
                           "GRANT SELECT ON sch.u TO child"));
  statements.push_back(Ddl("GRANT USAGE ON SCHEMA sch TO ROLE child",
                           "GRANT USAGE ON SCHEMA sch TO child"));
  statements.push_back(Ddl("GRANT SELECT, UPDATE ON SEQUENCE S TO ROLE child",
                           "GRANT SELECT, UPDATE ON SEQUENCE s TO child"));
  statements.push_back(Ddl("GRANT SELECT ON CHANGE STREAM CS TO ROLE child",
                           "GRANT SELECT ON CHANGE STREAM cs TO child"));
  statements.push_back(
      Ddl("GRANT EXECUTE ON TABLE FUNCTION READ_CS TO ROLE child",
          "GRANT EXECUTE ON FUNCTION spanner.read_json_cs TO child"));
  statements.push_back(Ddl("GRANT ROLE parent TO ROLE child",
                           "GRANT parent TO child"));
  statements.push_back(Ddl("REVOKE USAGE ON SCHEMA DEFAULT FROM ROLE public",
                           "REVOKE USAGE ON SCHEMA public FROM public"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  EXPECT_TRUE(schema->grants()->public_default_schema_usage_revoked());

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> printed,
                                 PrintDDLStatements(schema.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto recreated, Create(printed));
  EXPECT_THAT(PrintDDLStatements(recreated.get()),
              googlesql_base::testing::IsOkAndHolds(printed));
  EXPECT_EQ(PrintedGrants(recreated.get()).size(), 8);
}

TEST_P(GrantsTest, DuplicateGrantsAndMissingRevokesHaveNoEffect) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT SELECT ON TABLE T TO ROLE child",
                           "GRANT SELECT ON t TO child"));
  statements.push_back(Ddl("GRANT SELECT ON TABLE T TO ROLE child",
                           "GRANT SELECT ON TABLE t TO child"));
  statements.push_back(Ddl("REVOKE INSERT ON TABLE T FROM ROLE child",
                           "REVOKE INSERT ON t FROM child"));
  statements.push_back(Ddl("REVOKE ROLE parent FROM ROLE child",
                           "REVOKE parent FROM child"));
  // A column-level revoke does not affect a table-level grant.
  statements.push_back(Ddl("REVOKE SELECT(A) ON TABLE T FROM ROLE child",
                           "REVOKE SELECT(a) ON t FROM child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  ASSERT_EQ(schema->grants()->privileges().size(), 1);
  EXPECT_EQ(schema->grants()->privileges()[0].column, nullptr);
  EXPECT_THAT(schema->grants()->memberships(), IsEmpty());
}

TEST_P(GrantsTest, RevokingATablePrivilegeRevokesItsColumnPrivileges) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT SELECT(K, A), UPDATE(A) ON TABLE T TO ROLE "
                           "child",
                           "GRANT SELECT(k, a), UPDATE(a) ON t TO child"));
  statements.push_back(Ddl("REVOKE SELECT ON TABLE T FROM ROLE child",
                           "REVOKE SELECT ON t FROM child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  ASSERT_EQ(schema->grants()->privileges().size(), 1);
  EXPECT_EQ(schema->grants()->privileges()[0].type, ddl::Privilege::UPDATE);
}

TEST_P(GrantsTest, GrantOnAllObjectsInSchema) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back("CREATE SCHEMA sch");
  statements.push_back(Ddl("CREATE TABLE sch.U (K INT64) PRIMARY KEY (K)",
                           "CREATE TABLE sch.u (k bigint PRIMARY KEY)"));
  statements.push_back(Ddl("CREATE TABLE sch.W (K INT64) PRIMARY KEY (K)",
                           "CREATE TABLE sch.w (k bigint PRIMARY KEY)"));
  statements.push_back(Ddl("GRANT SELECT ON ALL TABLES IN SCHEMA sch TO ROLE "
                           "child",
                           "GRANT SELECT ON ALL TABLES IN SCHEMA sch TO child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  ASSERT_EQ(schema->grants()->privileges().size(), 2);

  // The grant does not cover tables created afterwards.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto updated,
      Update(schema.get(), {Ddl("CREATE TABLE sch.X (K INT64) PRIMARY KEY (K)",
                                "CREATE TABLE sch.x (k bigint PRIMARY KEY)")}));
  EXPECT_EQ(updated->grants()->privileges().size(), 2);
}

TEST_P(GrantsTest, DroppingAnObjectRevokesItsPrivileges) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT SELECT(A, B), INSERT ON TABLE T TO ROLE child",
                           "GRANT SELECT(a, b), INSERT ON t TO child"));
  statements.push_back(Ddl("GRANT SELECT ON VIEW V TO ROLE child",
                           "GRANT SELECT ON v TO child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  ASSERT_EQ(schema->grants()->privileges().size(), 4);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto without_column,
      Update(schema.get(), {Ddl("ALTER TABLE T DROP COLUMN B",
                                "ALTER TABLE t DROP COLUMN b")}));
  EXPECT_EQ(without_column->grants()->privileges().size(), 3);

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto without_table,
      Update(without_column.get(), {"DROP VIEW " + Ddl("V", "v"),
                                    "DROP TABLE " + Ddl("T", "t")}));
  EXPECT_THAT(without_table->grants()->privileges(), IsEmpty());
}

TEST_P(GrantsTest, InvalidGrants) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(BaseSchema()));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT SELECT ON TABLE T TO ROLE nobody",
                                        "GRANT SELECT ON t TO nobody")}),
              StatusIs(error::DatabaseRoleNotFound("nobody")));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT SELECT ON TABLE X TO ROLE child",
                                        "GRANT SELECT ON x TO child")}),
              StatusIs(error::TableNotFound(Ddl("X", "x"))));
  EXPECT_THAT(
      Update(schema.get(), {Ddl("GRANT SELECT(Z) ON TABLE T TO ROLE child",
                                "GRANT SELECT(z) ON t TO child")}),
      StatusIs(error::ColumnNotFound(Ddl("T", "t"), Ddl("Z", "z"))));
  if (!IsPostgreSQL()) {
    // Object names must use the case they were created with.
    EXPECT_THAT(Update(schema.get(), {"GRANT SELECT ON TABLE t TO ROLE child"}),
                StatusIs(error::TableNotFound("t")));
    EXPECT_THAT(Update(schema.get(), {"GRANT INSERT ON VIEW V TO ROLE child"}),
                StatusIs(error::InvalidPrivilegeForObject("INSERT", "view",
                                                          "V")));
    EXPECT_THAT(Update(schema.get(), {"GRANT SELECT(K) ON VIEW V TO ROLE child"}),
                StatusIs(error::ColumnPrivilegeNotAllowed("SELECT", "view",
                                                          "V")));
    EXPECT_THAT(
        Update(schema.get(), {"GRANT USAGE ON TABLE T TO ROLE child"}),
        StatusIs(error::InvalidPrivilegeForObject("USAGE", "table", "T")));
  } else {
    EXPECT_THAT(Update(schema.get(), {"GRANT INSERT ON v TO child"}),
                StatusIs(error::InvalidPrivilegeForObject("INSERT", "view",
                                                          "v")));
  }
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT DELETE(A) ON TABLE T TO ROLE "
                                        "child",
                                        "GRANT DELETE(a) ON t TO child")}),
              StatusIs(error::ColumnPrivilegeNotAllowed("DELETE", "table",
                                                        Ddl("T", "t"))));
  EXPECT_THAT(
      Update(schema.get(), {Ddl("GRANT INSERT(G) ON TABLE T TO ROLE child",
                                "GRANT INSERT(g) ON t TO child")}),
      StatusIs(error::PrivilegeOnGeneratedColumn("INSERT", Ddl("T", "t"),
                                                 Ddl("G", "g"))));
  EXPECT_THAT(Update(schema.get(),
                     {Ddl("GRANT SELECT ON TABLE T TO ROLE spanner_info_reader",
                          "GRANT SELECT ON t TO spanner_info_reader")}),
              StatusIs(error::CannotGrantPrivilegesToSystemRole(
                  "spanner_info_reader")));
  EXPECT_THAT(
      Update(schema.get(),
             {Ddl("GRANT SELECT ON TABLE FUNCTION READ_X TO ROLE child",
                  "GRANT SELECT ON FUNCTION spanner.read_json_x TO child")}),
      StatusIs(error::TableValuedFunctionNotFound(
          Ddl("READ_X", "read_json_x"))));
}

TEST_P(GrantsTest, InvalidMemberships) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(BaseSchema()));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT ROLE public TO ROLE child",
                                        "GRANT public TO child")}),
              StatusIs(error::CannotGrantMembershipInPublicRole()));
  EXPECT_THAT(
      Update(schema.get(), {Ddl("GRANT ROLE child TO ROLE spanner_sys_reader",
                                "GRANT child TO spanner_sys_reader")}),
      StatusIs(error::SystemRoleCannotBeMember("spanner_sys_reader")));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT ROLE child TO ROLE child",
                                        "GRANT child TO child")}),
              StatusIs(error::RoleMembershipCycle("child", "child")));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT ROLE parent TO ROLE child",
                                        "GRANT parent TO child"),
                                    Ddl("GRANT ROLE child TO ROLE parent",
                                        "GRANT child TO parent")}),
              StatusIs(error::RoleMembershipCycle("child", "parent")));
  EXPECT_THAT(Update(schema.get(), {Ddl("GRANT ROLE nobody TO ROLE child",
                                        "GRANT nobody TO child")}),
              StatusIs(error::DatabaseRoleNotFound("nobody")));
}

TEST_P(GrantsTest, DropRole) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT ROLE parent TO ROLE child",
                           "GRANT parent TO child"));
  statements.push_back(Ddl("GRANT SELECT ON TABLE T TO ROLE child",
                           "GRANT SELECT ON t TO child"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));

  EXPECT_THAT(Update(schema.get(), {"DROP ROLE spanner_info_reader"}),
              StatusIs(error::CannotDropSystemRole("spanner_info_reader")));
  EXPECT_THAT(Update(schema.get(), {"DROP ROLE child"}),
              StatusIs(error::CannotDropRoleWithPrivileges("child")));

  // Dropping a role removes its memberships in both directions.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto dropped,
                                 Update(schema.get(), {"DROP ROLE parent"}));
  EXPECT_THAT(dropped->grants()->memberships(), IsEmpty());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      dropped, Update(dropped.get(), {Ddl("REVOKE SELECT ON TABLE T FROM ROLE "
                                          "child",
                                          "REVOKE SELECT ON t FROM child"),
                                      "DROP ROLE child"}));
  EXPECT_EQ(dropped->FindRole("child"), nullptr);
}

TEST_P(GrantsTest, RoleNamesAndLimit) {
  EXPECT_THAT(Create({"CREATE ROLE spanner_admin"}),
              StatusIs(error::InvalidSchemaName("Role", "spanner_admin")));
  if (!IsPostgreSQL()) {
    EXPECT_THAT(Create({"CREATE ROLE public"}),
                StatusIs(error::InvalidSchemaName("Role", "public")));
  }

  std::vector<std::string> statements;
  for (int i = 0; i < limits::kMaxRolesPerDatabase; ++i) {
    statements.push_back(absl::StrCat("CREATE ROLE r", i));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Create(statements));
  EXPECT_THAT(Update(schema.get(), {"CREATE ROLE extra"}),
              StatusIs(error::TooManyRolesPerDatabase(
                  "extra", limits::kMaxRolesPerDatabase)));
}

TEST_P(GrantsTest, ReplayedGrantsThatNoLongerValidateAreSkipped) {
  std::vector<std::string> statements = BaseSchema();
  statements.push_back(Ddl("GRANT SELECT ON TABLE T TO ROLE nobody",
                           "GRANT SELECT ON t TO nobody"));
  statements.push_back(Ddl("GRANT SELECT ON TABLE T TO ROLE child",
                           "GRANT SELECT ON t TO child"));
  statements.push_back("DROP ROLE child");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto schema, Replay(statements));
  EXPECT_EQ(schema->FindRole("child"), nullptr);
  EXPECT_THAT(schema->grants()->privileges(), IsEmpty());
}

}  // namespace

}  // namespace test
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
