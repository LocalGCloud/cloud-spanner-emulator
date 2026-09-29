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

#include "frontend/common/list_filter.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "google/longrunning/operations.pb.h"
#include "google/protobuf/map.h"
#include "google/spanner/admin/database/v1/backup.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "googlesql/base/testing/status_matchers.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace operations_api = ::google::longrunning;

using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;

// Evaluates `expression`, where a predicate is true when its field is in
// `true_fields`.
absl::StatusOr<bool> Evaluate(std::string_view expression,
                              const std::set<std::string>& true_fields) {
  absl::StatusOr<ListFilter> filter = ListFilter::Parse(expression);
  if (!filter.ok()) return filter.status();
  return filter->Matches([&true_fields](std::string_view field,
                                        std::string_view,
                                        std::string_view) -> absl::StatusOr<bool> {
    return true_fields.count(std::string(field)) > 0;
  });
}

absl::StatusOr<bool> Matches(std::string_view expression,
                             const operations_api::Operation& operation) {
  absl::StatusOr<ListFilter> filter = ListFilter::Parse(expression);
  if (!filter.ok()) return filter.status();
  return FilterMatchesOperation(*filter, operation);
}

TEST(ListFilterTest, TokenizesPredicates) {
  absl::StatusOr<ListFilter> filter = ListFilter::Parse(
      "Name:\"a \\\"b\\\" \\\\c\" size>=10 x!='it\\'s' y<=1 z<2 w>3 "
      "metadata.@type=type.googleapis.com/x.Y (v = \"AND\")");
  ASSERT_TRUE(filter.ok()) << filter.status();
  std::vector<std::tuple<std::string, std::string, std::string>> predicates;
  ASSERT_THAT(filter->Matches([&predicates](std::string_view field,
                                            std::string_view op,
                                            std::string_view value)
                                  -> absl::StatusOr<bool> {
    predicates.emplace_back(field, op, value);
    return true;
  }),
              IsOkAndHolds(true));
  EXPECT_THAT(
      predicates,
      ElementsAre(std::make_tuple("name", ":", "a \"b\" \\c"),
                  std::make_tuple("size", ">=", "10"),
                  std::make_tuple("x", "!=", "it's"),
                  std::make_tuple("y", "<=", "1"),
                  std::make_tuple("z", "<", "2"),
                  std::make_tuple("w", ">", "3"),
                  std::make_tuple("metadata.@type", "=",
                                  "type.googleapis.com/x.Y"),
                  std::make_tuple("v", "=", "AND")));
}

TEST(ListFilterTest, EmptyFilterMatchesEverything) {
  EXPECT_THAT(Evaluate("", {}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate(" \t\n", {}), IsOkAndHolds(true));
}

TEST(ListFilterTest, OrBindsTighterThanAnd) {
  // AIP-160: a AND (b OR c), not (a AND b) OR c.
  EXPECT_THAT(Evaluate("a:1 AND b:1 OR c:1", {"c"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate("a:1 AND b:1 OR c:1", {"a", "c"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("a:1 OR b:1 AND c:1", {"a"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate("a:1 OR b:1 AND c:1", {"b", "c"}), IsOkAndHolds(true));
  // Implicit AND also binds looser than OR.
  EXPECT_THAT(Evaluate("a:1 b:1 OR c:1", {"a", "c"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("a:1 b:1 OR c:1", {"c"}), IsOkAndHolds(false));
}

TEST(ListFilterTest, DocumentedBackupOperationExample) {
  // The ListBackupOperations example means (A AND B OR C AND D) AND E.
  constexpr std::string_view kExample =
      "((a:1) AND (b:1)) OR ((c:1) AND (d:1)) AND (e:1)";
  EXPECT_THAT(Evaluate(kExample, {"a", "b"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate(kExample, {"c", "d"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate(kExample, {"a", "b", "e"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate(kExample, {"c", "d", "e"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate(kExample, {"a", "d", "e"}), IsOkAndHolds(false));
}

TEST(ListFilterTest, NotParenthesesAndImplicitAnd) {
  EXPECT_THAT(Evaluate("a:1 b:1", {"a"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate("a:1 b:1", {"a", "b"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("(a:1) (b:1)", {"a", "b"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("NOT a:1", {}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("NOT a:1 OR b:1", {"a"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate("NOT a:1 OR b:1", {"a", "b"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("NOT (a:1 OR b:1)", {"b"}), IsOkAndHolds(false));
  EXPECT_THAT(Evaluate("NOT NOT a:1", {"a"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("(a:1 AND b:1) OR c:1", {"c"}), IsOkAndHolds(true));
  // Keywords and field names are case-insensitive.
  EXPECT_THAT(Evaluate("A:1 and b:1 Or c:1", {"a", "c"}), IsOkAndHolds(true));
  EXPECT_THAT(Evaluate("not a:1", {"a"}), IsOkAndHolds(false));
  // A quoted keyword is a value, not an operator.
  EXPECT_THAT(Evaluate("a:\"OR\"", {"a"}), IsOkAndHolds(true));
}

TEST(ListFilterTest, RejectsInvalidSyntax) {
  for (const std::string& expression : std::vector<std::string>{
           "a", "a:", ":a", "a b", "a:1 AND", "AND a:1", "a:1 OR", "OR a:1",
           "a:1 AND AND b:1", "(a:1", "a:1)", "()", "a ! b", "a:(", "a:=",
           "\"a\":1", "a \"=\" 1", "a:\"x", "a:'x", "a:\"x\\y\"", "NOT",
           "= = 1", std::string("a:1\0", 4)}) {
    EXPECT_THAT(ListFilter::Parse(expression),
                StatusIs(absl::StatusCode::kInvalidArgument))
        << expression;
  }
}

TEST(ListFilterTest, EnforcesLimits) {
  EXPECT_TRUE(ListFilter::Parse(std::string(8190, 'a') + ":1").ok());
  EXPECT_THAT(ListFilter::Parse(std::string(8191, 'a') + ":1"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  std::vector<std::string> predicates(170, "a:1");
  EXPECT_TRUE(ListFilter::Parse(absl::StrJoin(predicates, " ")).ok());
  predicates.push_back("a:1");
  EXPECT_THAT(ListFilter::Parse(absl::StrJoin(predicates, " ")),
              StatusIs(absl::StatusCode::kInvalidArgument));

  EXPECT_TRUE(ListFilter::Parse(std::string(32, '(') + "a:1" +
                                std::string(32, ')'))
                  .ok());
  EXPECT_THAT(ListFilter::Parse(std::string(33, '(') + "a:1" +
                                std::string(33, ')')),
              StatusIs(absl::StatusCode::kInvalidArgument));
  std::string nots;
  for (int i = 0; i < 33; ++i) nots += "NOT ";
  EXPECT_THAT(ListFilter::Parse(nots + "a:1"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ListFilterTest, ResolvesEveryPredicate) {
  absl::StatusOr<ListFilter> filter = ListFilter::Parse("a:1 OR bad:1");
  ASSERT_TRUE(filter.ok()) << filter.status();
  EXPECT_THAT(filter->Matches([](std::string_view field, std::string_view,
                                 std::string_view) -> absl::StatusOr<bool> {
    if (field == "bad") return absl::InvalidArgumentError("bad field");
    return true;
  }),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ListFilterTest, ComparesStringsIgnoringCase) {
  EXPECT_TRUE(CompareString("Howl", "HOWL", "="));
  EXPECT_FALSE(CompareString("Howl", "HOWL", "!="));
  EXPECT_TRUE(CompareString("projects/p/instances/Howl-1", "howl", ":"));
  EXPECT_FALSE(CompareString("projects/p/instances/i", "howl", ":"));
  EXPECT_TRUE(CompareString("x", "*", ":"));
  EXPECT_FALSE(CompareString("", "*", ":"));
  EXPECT_TRUE(CompareString("a", "B", "<"));
  EXPECT_TRUE(CompareOrdered(2, 1, ">="));
  EXPECT_FALSE(CompareOrdered(2, 1, "<="));
}

TEST(ListFilterTest, MatchesLabels) {
  google::protobuf::Map<std::string, std::string> labels;
  labels["env"] = "Dev-1";
  labels["empty"] = "";
  EXPECT_THAT(MatchLabel(labels, "env", ":", "*"), IsOkAndHolds(true));
  EXPECT_THAT(MatchLabel(labels, "empty", ":", "*"), IsOkAndHolds(true));
  EXPECT_THAT(MatchLabel(labels, "team", ":", "*"), IsOkAndHolds(false));
  EXPECT_THAT(MatchLabel(labels, "env", ":", "dev"), IsOkAndHolds(true));
  EXPECT_THAT(MatchLabel(labels, "env", "=", "dev-1"), IsOkAndHolds(true));
  EXPECT_THAT(MatchLabel(labels, "env", "=", "dev"), IsOkAndHolds(false));
  EXPECT_THAT(MatchLabel(labels, "team", "=", "dev"), IsOkAndHolds(false));
  EXPECT_THAT(MatchLabel(labels, "team", "!=", "dev"), IsOkAndHolds(true));
  EXPECT_THAT(MatchLabel(labels, "", ":", "*"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

class OperationFilterTest : public testing::Test {
 protected:
  void SetUp() override {
    database_api::CreateBackupMetadata create;
    create.set_name("projects/p/instances/i/backups/backup_howl");
    create.set_database("projects/p/instances/i/databases/test_db");
    create.mutable_progress()->set_progress_percent(100);
    create.mutable_progress()->mutable_start_time()->set_seconds(
        kBeforeExample);
    create_backup_.set_name(
        "projects/p/instances/i/backups/backup_howl/operations/create");
    create_backup_.set_done(true);
    create_backup_.mutable_metadata()->PackFrom(create);
    database_api::Backup backup;
    backup.set_name(create.name());
    backup.set_size_bytes(4096);
    backup.set_state(database_api::Backup::READY);
    create_backup_.mutable_response()->PackFrom(backup);

    database_api::CopyBackupMetadata copy;
    copy.set_name("projects/p/instances/i/backups/copy");
    copy.set_source_backup("projects/p/instances/i/backups/test_bkp");
    copy.mutable_progress()->mutable_start_time()->set_seconds(kAfterExample);
    copy_backup_.set_name(
        "projects/p/instances/i/backups/copy/operations/copy");
    copy_backup_.set_done(true);
    copy_backup_.mutable_metadata()->PackFrom(copy);
    copy_backup_.mutable_error()->set_code(9);
    copy_backup_.mutable_error()->set_message("Source backup is Not Ready");

    database_api::RestoreDatabaseMetadata restore;
    restore.set_name("projects/p/instances/i/databases/restored_howl");
    restore.set_source_type(database_api::BACKUP);
    restore.mutable_backup_info()->set_backup(create.name());
    restore_.set_name(
        "projects/p/instances/i/databases/restored_howl/operations/restore");
    restore_.mutable_metadata()->PackFrom(restore);

    database_api::UpdateDatabaseDdlMetadata ddl;
    ddl.set_database("projects/p/instances/i/databases/d");
    ddl.add_statements("CREATE TABLE T (K INT64) PRIMARY KEY (K)");
    ddl.add_statements("CREATE INDEX I ON T (K)");
    ddl.add_progress()->set_progress_percent(100);
    ddl.add_progress()->set_progress_percent(40);
    ddl_.set_name("projects/p/instances/i/databases/d/operations/ddl");
    ddl_.mutable_metadata()->PackFrom(ddl);
  }

  static constexpr int64_t kBeforeExample = 1522248600 - 60;
  static constexpr int64_t kAfterExample = 1522248600 + 60;

  operations_api::Operation create_backup_;
  operations_api::Operation copy_backup_;
  operations_api::Operation restore_;
  operations_api::Operation ddl_;
};

TEST_F(OperationFilterTest, MatchesTopLevelFields) {
  EXPECT_THAT(Matches("done:true", create_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("done = FALSE", create_backup_), IsOkAndHolds(false));
  EXPECT_THAT(Matches("done = false", restore_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("done:*", restore_), IsOkAndHolds(false));
  EXPECT_THAT(Matches("NAME:HOWL", create_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("name:howl", copy_backup_), IsOkAndHolds(false));
  EXPECT_THAT(Matches("error:*", copy_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("error:*", create_backup_), IsOkAndHolds(false));
  EXPECT_THAT(Matches("NOT error:*", create_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("error:\"not ready\"", copy_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("error.code = 9", copy_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("error.code > 9", copy_backup_), IsOkAndHolds(false));
  EXPECT_THAT(Matches("error.message:ready", copy_backup_),
              IsOkAndHolds(true));
}

TEST_F(OperationFilterTest, MatchesTypesAndMetadataPaths) {
  const std::string create_type =
      "type.googleapis.com/google.spanner.admin.database.v1."
      "CreateBackupMetadata";
  EXPECT_THAT(Matches("metadata.@type=" + create_type, create_backup_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.@type=" + create_type, copy_backup_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.@type:*", restore_), IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.database:prod", create_backup_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.database:TEST_DB", create_backup_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.progress.start_time < "
                      "\"2018-03-28T14:50:00Z\"",
                      create_backup_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.progress.start_time < "
                      "\"2018-03-28T14:50:00Z\"",
                      copy_backup_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.progress.progress_percent = 100",
                      create_backup_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.source_type:backup AND "
                      "metadata.backup_info.backup:backup_howl AND "
                      "metadata.name:restored_howl",
                      restore_),
              IsOkAndHolds(true));
  // Path segments may use lowerCamelCase.
  EXPECT_THAT(Matches("metadata.backupInfo.backup:howl", restore_),
              IsOkAndHolds(true));
  // A field that the packed type lacks does not match.
  EXPECT_THAT(Matches("metadata.source_backup:test", create_backup_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.progress.start_time:*", restore_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("response.@type:Backup AND response.size_bytes > 1024 "
                      "AND response.state = ready",
                      create_backup_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("response:*", restore_), IsOkAndHolds(false));
}

TEST_F(OperationFilterTest, MatchesRepeatedFields) {
  EXPECT_THAT(Matches("metadata.statements:\"create index\"", ddl_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.progress.progress_percent = 40", ddl_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.progress.progress_percent != 40", ddl_),
              IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.progress.progress_percent != 50", ddl_),
              IsOkAndHolds(true));
  EXPECT_THAT(Matches("metadata.commit_timestamps:*", ddl_),
              IsOkAndHolds(false));
}

TEST_F(OperationFilterTest, DocumentedExample) {
  const std::string example =
      "((metadata.@type=type.googleapis.com/"
      "google.spanner.admin.database.v1.CreateBackupMetadata) AND "
      "(metadata.database:test_db)) OR "
      "((metadata.@type=type.googleapis.com/"
      "google.spanner.admin.database.v1.CopyBackupMetadata) AND "
      "(metadata.source_backup:test_bkp)) AND (error:*)";
  EXPECT_THAT(Matches(example, create_backup_), IsOkAndHolds(false));
  EXPECT_THAT(Matches(example, copy_backup_), IsOkAndHolds(true));
  EXPECT_THAT(Matches(example, restore_), IsOkAndHolds(false));
}

TEST_F(OperationFilterTest, RejectsUnknownFieldsAndInvalidValues) {
  const operations_api::Operation empty;
  for (const std::string& expression : std::vector<std::string>{
           "unknown:1", "metadata_type:x", "error.unknown:1", "name.x:1",
           "metadata..x:1", "done = maybe"}) {
    EXPECT_THAT(Matches(expression, empty),
                StatusIs(absl::StatusCode::kInvalidArgument))
        << expression;
  }
  EXPECT_THAT(Matches("metadata.unknown:1", empty), IsOkAndHolds(false));
  EXPECT_THAT(Matches("metadata.progress.start_time < \"yesterday\"",
                      create_backup_),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Matches("metadata.progress.progress_percent > high",
                      create_backup_),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(Matches("metadata.progress:x", create_backup_),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(ListPageTokenTest, BindsCursorToFilter) {
  const std::string token =
      MakeListPageToken("name:howl", "projects/p/instances/i");
  EXPECT_THAT(ParseListPageToken(token, "name:howl"),
              IsOkAndHolds("projects/p/instances/i"));
  EXPECT_THAT(ParseListPageToken(token, ""),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseListPageToken(token, "name:howl2"),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseListPageToken(MakeListPageToken("", "x"), ""),
              IsOkAndHolds("x"));
  EXPECT_THAT(ParseListPageToken("projects/p/instances/i", ""),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseListPageToken(std::string(16 * 1024 + 1, 'A'), ""),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
