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

#include "frontend/collections/database_manager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/match.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/schema.h"
#include "backend/transaction/read_only_transaction.h"
#include "frontend/entities/database.h"
#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {


namespace {

class TempDirectory {
 public:
  TempDirectory()
      : path_(std::filesystem::temp_directory_path() /
              absl::StrCat(
                  "spanner-database-manager-",
                  std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedDataDir {
 public:
  explicit ScopedDataDir(const std::string& path)
      : previous_(absl::GetFlag(FLAGS_data_dir)) {
    absl::SetFlag(&FLAGS_data_dir, path);
  }

  ~ScopedDataDir() { absl::SetFlag(&FLAGS_data_dir, previous_); }

 private:
  std::string previous_;
};

}  // namespace
class DatabaseManagerTest : public testing::Test {
 protected:
  DatabaseManagerTest()
      : database_manager_(&clock_),
        database_uri_(
            "projects/test-p/instances/test-instance/databases/test-database") {
  }

  Clock clock_;
  DatabaseManager database_manager_;
  const std::string database_uri_;
  const backend::SchemaChangeOperation empty_schema_operation_;
};

TEST_F(DatabaseManagerTest, AcceptsFreshPersistentHierarchy) {
  TempDirectory temp;
  GOOGLESQL_EXPECT_OK(DatabaseManager::MigrateLegacyStorageDirectories(
      temp.path().string(), {}));
  GOOGLESQL_EXPECT_OK(DatabaseManager::ReconcileDeletedDatabaseDirectories(
      temp.path().string(), {}));
  GOOGLESQL_EXPECT_OK(DatabaseManager::CleanupOrphanedRestoreDirectories(
      temp.path().string(), {}));
}

TEST_F(DatabaseManagerTest, RestoresDurableDdlRollbackCheckpoint) {
  TempDirectory temp;
  const std::string resource =
      "projects/p/instances/i/databases/database";
  const std::string operation =
      resource + "/operations/ddl_rollback";
  const std::filesystem::path storage = temp.path() / resource / "storage";
  std::filesystem::create_directories(storage);
  std::ofstream(storage / "marker") << "mutated";
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string checkpoint,
      DatabaseManager::DdlRollbackCheckpointDirectory(
          temp.path().string(), resource, operation));
  std::filesystem::create_directories(checkpoint);
  std::ofstream(std::filesystem::path(checkpoint) / "marker") << "original";

  GOOGLESQL_EXPECT_OK(DatabaseManager::RestoreDdlRollbackCheckpoint(
      temp.path().string(), resource, operation));

  std::ifstream restored(storage / "marker");
  std::string contents;
  restored >> contents;
  EXPECT_EQ(contents, "original");
  GOOGLESQL_EXPECT_OK(DatabaseManager::RemoveDdlRollbackCheckpoints(
      temp.path().string(), resource));
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(checkpoint).parent_path().parent_path()));
}

TEST_F(DatabaseManagerTest, MigratesLegacyStorageDirectoryOnce) {
  TempDirectory temp;
  const std::filesystem::path legacy =
      temp.path() / "shared-database";
  std::filesystem::create_directories(legacy / "storage");
  std::ofstream(legacy / "storage" / "marker") << "legacy";
  const std::string resource =
      "projects/p/instances/i/databases/shared-database";

  GOOGLESQL_EXPECT_OK(DatabaseManager::MigrateLegacyStorageDirectories(
      temp.path().string(), {resource}));

  const std::filesystem::path scoped =
      temp.path() / resource;
  EXPECT_FALSE(std::filesystem::exists(legacy));
  EXPECT_TRUE(std::filesystem::exists(scoped / "storage" / "marker"));
  GOOGLESQL_EXPECT_OK(DatabaseManager::MigrateLegacyStorageDirectories(
      temp.path().string(), {resource}));
}

TEST_F(DatabaseManagerTest, RejectsAmbiguousLegacyStorageDirectory) {
  TempDirectory temp;
  std::filesystem::create_directories(
      temp.path() / "shared-database" / "storage");
  const std::string first =
      "projects/p/instances/i1/databases/shared-database";
  const std::string second =
      "projects/p/instances/i2/databases/shared-database";

  EXPECT_THAT(
      DatabaseManager::MigrateLegacyStorageDirectories(
          temp.path().string(), {first, second}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::AllOf(testing::HasSubstr(first),
                         testing::HasSubstr(second))));
  EXPECT_TRUE(
      std::filesystem::exists(temp.path() / "shared-database"));
}

TEST_F(DatabaseManagerTest, RejectsLegacyAndScopedStorageConflict) {
  TempDirectory temp;
  const std::string resource =
      "projects/p/instances/i/databases/shared-database";
  const std::filesystem::path legacy =
      temp.path() / "shared-database";
  const std::filesystem::path scoped = temp.path() / resource;
  std::filesystem::create_directories(legacy / "storage");
  std::filesystem::create_directories(scoped / "storage");

  EXPECT_THAT(
      DatabaseManager::MigrateLegacyStorageDirectories(
          temp.path().string(), {resource}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::AllOf(testing::HasSubstr(legacy.string()),
                         testing::HasSubstr(scoped.string()),
                         testing::HasSubstr(resource))));
}

TEST_F(DatabaseManagerTest, CleansIncompleteDatabaseAndRestoreDirectories) {
  TempDirectory temp;
  const std::string persisted =
      "projects/p/instances/i/databases/persisted";
  const std::filesystem::path persisted_root = temp.path() / persisted;
  const std::filesystem::path orphan_root =
      temp.path() / "projects/p/instances/i/databases/orphan";
  const std::filesystem::path staging_root =
      temp.path() /
      "projects/p/instances/i/databases/restored.restore-staging-123";
  const std::filesystem::path unrelated_root =
      temp.path() / "projects/p/instances/i/databases/unrelated";
  std::filesystem::create_directories(persisted_root);
  std::filesystem::create_directories(orphan_root);
  std::filesystem::create_directories(staging_root);
  std::filesystem::create_directories(unrelated_root);
  std::ofstream(persisted_root / ".restore-in-progress") << persisted;
  std::ofstream(orphan_root / ".restore-in-progress") << "orphan";
  std::ofstream(staging_root / ".restore-in-progress") << persisted;

  GOOGLESQL_EXPECT_OK(DatabaseManager::CleanupOrphanedRestoreDirectories(
      temp.path().string(), {persisted}));

  EXPECT_TRUE(std::filesystem::exists(persisted_root));
  EXPECT_TRUE(
      std::filesystem::exists(persisted_root / ".restore-in-progress"));
  EXPECT_FALSE(std::filesystem::exists(orphan_root));
  EXPECT_FALSE(std::filesystem::exists(staging_root));
  EXPECT_FALSE(std::filesystem::exists(unrelated_root));
  GOOGLESQL_EXPECT_OK(DatabaseManager::CompleteRecoveredRestoreDirectories(
      temp.path().string(), {persisted}));
  EXPECT_TRUE(std::filesystem::exists(persisted_root));
  EXPECT_FALSE(
      std::filesystem::exists(persisted_root / ".restore-in-progress"));
}

TEST_F(DatabaseManagerTest, RejectsCommittedDatabaseRootMissingMetadata) {
  TempDirectory temp;
  const std::string database_uri =
      "projects/p/instances/i/databases/committed";
  const std::filesystem::path database_root = temp.path() / database_uri;
  std::filesystem::create_directories(database_root / "storage");
  GOOGLESQL_EXPECT_OK(DatabaseManager::MarkDatabaseMetadataCommitted(
      temp.path().string(), database_uri));

  EXPECT_THAT(
      DatabaseManager::CleanupOrphanedRestoreDirectories(
          temp.path().string(), {}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::AllOf(testing::HasSubstr("committed data"),
                         testing::HasSubstr(database_uri))));
  EXPECT_TRUE(std::filesystem::exists(database_root));
}

TEST_F(DatabaseManagerTest, QuarantinedDatabaseDoesNotBlockNextStartup) {
  TempDirectory temp;
  const std::string database_uri =
      "projects/p/instances/i/databases/corrupted";
  const std::filesystem::path database_root = temp.path() / database_uri;
  std::filesystem::create_directories(database_root / "storage");
  std::ofstream(database_root / "storage" / "CURRENT") << "garbage";
  GOOGLESQL_ASSERT_OK(DatabaseManager::MarkDatabaseMetadataCommitted(
      temp.path().string(), database_uri));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::optional<std::string> quarantine_path,
      DatabaseManager::QuarantineDatabaseDirectory(
          temp.path().string(), database_uri, absl::FromUnixMicros(42)));
  ASSERT_TRUE(quarantine_path.has_value());
  EXPECT_EQ(
      *quarantine_path,
      (temp.path() / ".quarantine" / "projects_p_instances_i_databases_corrupted-42")
          .string());

  // The whole database folder moves, markers included, so nothing is left
  // behind for the next startup to trip over.
  EXPECT_FALSE(std::filesystem::exists(database_root));
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(*quarantine_path) / "storage" / "CURRENT"));
  EXPECT_TRUE(std::filesystem::exists(
      std::filesystem::path(*quarantine_path) / ".metadata-committed"));

  // The next startup runs with the database gone from metadata.
  GOOGLESQL_EXPECT_OK(DatabaseManager::ReconcileDeletedDatabaseDirectories(
      temp.path().string(), {}));
  GOOGLESQL_EXPECT_OK(DatabaseManager::CleanupOrphanedRestoreDirectories(
      temp.path().string(), {}));

  // A second quarantine of the same database finds nothing to move.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      quarantine_path,
      DatabaseManager::QuarantineDatabaseDirectory(
          temp.path().string(), database_uri, absl::FromUnixMicros(43)));
  EXPECT_FALSE(quarantine_path.has_value());
}

TEST_F(DatabaseManagerTest, ReconcilesDatabaseDeletionCrashWindows) {
  TempDirectory temp;
  const std::string retained =
      "projects/p/instances/i/databases/retained";
  const std::string deleted =
      "projects/p/instances/i/databases/deleted";
  const std::filesystem::path retained_root = temp.path() / retained;
  const std::filesystem::path deleted_root = temp.path() / deleted;
  std::filesystem::create_directories(retained_root / "storage");
  std::filesystem::create_directories(deleted_root / "storage");

  GOOGLESQL_EXPECT_OK(DatabaseManager::MarkDatabaseForDeletion(
      temp.path().string(), retained));
  GOOGLESQL_EXPECT_OK(DatabaseManager::MarkDatabaseForDeletion(
      temp.path().string(), deleted));
  EXPECT_TRUE(
      std::filesystem::exists(retained_root / ".delete-in-progress"));
  EXPECT_TRUE(std::filesystem::exists(deleted_root / ".delete-in-progress"));

  GOOGLESQL_EXPECT_OK(
      DatabaseManager::ReconcileDeletedDatabaseDirectories(
          temp.path().string(), {retained}));
  EXPECT_TRUE(std::filesystem::exists(retained_root));
  EXPECT_FALSE(
      std::filesystem::exists(retained_root / ".delete-in-progress"));
  EXPECT_FALSE(std::filesystem::exists(deleted_root));
}

TEST_F(DatabaseManagerTest, DeletionMarkerBlocksRecreationUntilCleanup) {
  TempDirectory temp;
  ScopedDataDir data_dir(temp.path().string());
  const std::string database_uri =
      "projects/p/instances/i/databases/deleting";
  const std::filesystem::path database_root = temp.path() / database_uri;
  std::filesystem::create_directories(database_root / "storage");
  std::ofstream(database_root / "storage" / "old-row") << "old data";
  GOOGLESQL_ASSERT_OK(DatabaseManager::MarkDatabaseForDeletion(
      temp.path().string(), database_uri));

  DatabaseManager persistent_manager(&clock_, temp.path().string());
  EXPECT_THAT(
      persistent_manager.ReserveDatabase(database_uri),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kFailedPrecondition,
          testing::HasSubstr("deletion is in progress")));
  EXPECT_TRUE(std::filesystem::exists(database_root / "storage" / "old-row"));

  GOOGLESQL_ASSERT_OK(backend::Database::DeletePersistentStorageDirectory(
      temp.path().string(), database_uri));
  EXPECT_FALSE(std::filesystem::exists(database_root));
  GOOGLESQL_EXPECT_OK(persistent_manager.ReserveDatabase(database_uri));
}

TEST_F(DatabaseManagerTest, DeletionMarkerAfterReservationBlocksBuild) {
  TempDirectory temp;
  ScopedDataDir data_dir(temp.path().string());
  const std::string database_uri =
      "projects/p/instances/i/databases/deleting-after-reserve";
  const std::filesystem::path database_root = temp.path() / database_uri;
  std::filesystem::create_directories(database_root / "storage");

  DatabaseManager persistent_manager(&clock_, temp.path().string());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<DatabaseManager::Creation> creation,
      persistent_manager.ReserveDatabase(database_uri));
  GOOGLESQL_ASSERT_OK(DatabaseManager::MarkDatabaseForDeletion(
      temp.path().string(), database_uri));
  EXPECT_THAT(
      creation->Build(backend::SchemaChangeOperation{},
                      backend::Database::IdCounterValues{}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kFailedPrecondition,
          testing::HasSubstr("deletion is in progress")));
}

TEST_F(DatabaseManagerTest, RejectsSymlinkedResourceHierarchyParents) {
  TempDirectory temp;
  TempDirectory outside;
  std::filesystem::create_directory_symlink(outside.path(),
                                            temp.path() / "projects");
  EXPECT_THAT(
      DatabaseManager::ReconcileDeletedDatabaseDirectories(
          temp.path().string(), {}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::HasSubstr("symbolic link")));

  std::filesystem::remove(temp.path() / "projects");
  std::filesystem::create_directories(temp.path() / "projects/p");
  std::filesystem::create_directory_symlink(
      outside.path(), temp.path() / "projects/p/instances");
  EXPECT_THAT(
      DatabaseManager::CleanupOrphanedRestoreDirectories(
          temp.path().string(), {}),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kDataLoss,
          testing::HasSubstr("symbolic link")));
}

TEST_F(DatabaseManagerTest, RejectsMismatchedPersistedRestoreMarker) {
  TempDirectory temp;
  const std::string persisted =
      "projects/p/instances/i/databases/persisted";
  const std::filesystem::path persisted_root = temp.path() / persisted;
  std::filesystem::create_directories(persisted_root);
  std::ofstream(persisted_root / ".restore-in-progress")
      << "projects/p/instances/i/databases/other";

  EXPECT_THAT(
      DatabaseManager::CleanupOrphanedRestoreDirectories(
          temp.path().string(), {persisted}),
      googlesql_base::testing::StatusIs(absl::StatusCode::kDataLoss,
                                        testing::HasSubstr(persisted)));
  EXPECT_TRUE(std::filesystem::exists(persisted_root));
}

TEST_F(DatabaseManagerTest, EqualDatabaseIdsUseIndependentScopedStorage) {
  TempDirectory temp;
  ScopedDataDir data_dir(temp.path().string());
  const std::string first =
      "projects/p/instances/i1/databases/shared-database";
  const std::string second =
      "projects/p/instances/i2/databases/shared-database";

  GOOGLESQL_ASSERT_OK(database_manager_.CreateDatabase(
      first, empty_schema_operation_));
  GOOGLESQL_ASSERT_OK(database_manager_.CreateDatabase(
      second, empty_schema_operation_));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string first_storage,
      backend::Database::PersistentStorageDirectory(temp.path().string(),
                                                    first));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string second_storage,
      backend::Database::PersistentStorageDirectory(temp.path().string(),
                                                    second));
  EXPECT_TRUE(std::filesystem::exists(first_storage));
  EXPECT_TRUE(std::filesystem::exists(second_storage));
}

TEST_F(DatabaseManagerTest, ReservationHidesDatabaseUntilPublication) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<DatabaseManager::Creation> creation,
      database_manager_.ReserveDatabase(database_uri_));
  EXPECT_THAT(database_manager_.GetDatabase(database_uri_),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kNotFound));
  EXPECT_THAT(database_manager_.ReserveDatabase(database_uri_),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kAlreadyExists));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      creation->Build(empty_schema_operation_,
                      backend::Database::IdCounterValues{}));
  EXPECT_THAT(database_manager_.GetDatabase(database_uri_),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kNotFound));
  GOOGLESQL_ASSERT_OK(creation->Publish());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> published,
      database_manager_.GetDatabase(database_uri_));
  EXPECT_EQ(published, database);
}

TEST_F(DatabaseManagerTest, AbandonedReservationCanBeRetried) {
  {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<DatabaseManager::Creation> creation,
        database_manager_.ReserveDatabase(database_uri_));
  }
  GOOGLESQL_ASSERT_OK(database_manager_.CreateDatabase(
      database_uri_, empty_schema_operation_));
}

TEST_F(DatabaseManagerTest, InitialSchemaUsesPersistedCreationTimestamp) {
  const absl::Time create_time = clock_.Now() - absl::Seconds(1);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto database, database_manager_.CreateDatabase(
          database_uri_, backend::SchemaChangeOperation{
                             .statements = {"CREATE TABLE T (K INT64 NOT NULL) PRIMARY KEY (K)",
                                            "CREATE CHANGE STREAM C FOR ALL"}},
          backend::Database::IdCounterValues{}, create_time));
  EXPECT_EQ(database->backend()->GetLatestSchema()->FindChangeStream("C")
                ->creation_time(), create_time);
}

TEST_F(DatabaseManagerTest, LegacyInitialTimestampDoesNotDuplicatePartitionsOnReplay) {
  TempDirectory temp;
  ScopedDataDir data_dir(temp.path().string());
  const absl::Time create_time = clock_.Now() - absl::Seconds(1);
  const std::vector<std::string> statements = {
      "CREATE TABLE T (K INT64 NOT NULL) PRIMARY KEY (K)",
      "CREATE CHANGE STREAM C FOR ALL"};
  backend::SchemaChangeOperation schema{.statements = statements};
  // Reproduce an older journal: resource creation was recorded before the
  // backend chose the timestamp used to write its initial partition rows.
  schema.schema_change_timestamp = clock_.Now();
  auto tokens = [](const std::shared_ptr<Database>& database) {
    std::set<std::string> result;
    auto txn = database->backend()->CreateReadOnlyTransaction(backend::ReadOnlyOptions());
    EXPECT_TRUE(txn.ok()) << txn.status();
    if (!txn.ok()) return result;
    backend::ReadArg read;
    read.change_stream_for_partition_table = "C";
    read.columns = {"partition_token"};
    read.key_set = backend::KeySet::All();
    std::unique_ptr<backend::RowCursor> cursor;
    auto status = (*txn)->Read(read, &cursor);
    EXPECT_TRUE(status.ok()) << status;
    if (!status.ok()) return result;
    while (cursor->Next()) result.insert(cursor->ColumnValue(0).string_value());
    return result;
  };
  std::set<std::string> original;
  backend::Database::IdCounterValues counters;
  {
    DatabaseManager manager(&clock_);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto database, manager.CreateDatabase(
        database_uri_, schema, backend::Database::IdCounterValues{}, create_time));
    original = tokens(database);
    ASSERT_EQ(original.size(), 2);
    counters = database->backend()->GetIdCounterValues();
  }
  schema.schema_change_timestamp = create_time;
  for (int restart = 0; restart < 2; ++restart) {
    DatabaseManager manager(&clock_);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto database, manager.CreateDatabase(
        database_uri_, schema, counters, create_time));
    EXPECT_EQ(tokens(database), original);
  }
}

TEST_F(DatabaseManagerTest, CreateNewDatabase) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_));
  EXPECT_EQ(database->database_uri(), database_uri_);
  EXPECT_EQ(database->backend()->dialect(),
            backend::database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseManagerTest, CreateNewPGDatabase) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(
          database_uri_,
          backend::SchemaChangeOperation{
              .database_dialect =
                  backend::database_api::DatabaseDialect::POSTGRESQL}));
  EXPECT_EQ(database->database_uri(), database_uri_);
  EXPECT_EQ(database->backend()->dialect(),
            backend::database_api::DatabaseDialect::POSTGRESQL);
}

TEST_F(DatabaseManagerTest, CreateExistingDatabaseUriFailsWithAlreadyExists) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_));
  EXPECT_THAT(
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(DatabaseManagerTest, GetExistingDatabase) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Database> actual_database,
                       database_manager_.GetDatabase(database_uri_));
  EXPECT_EQ(actual_database->database_uri(), database_uri_);
}

TEST_F(DatabaseManagerTest, GetNonExistingDatabaseReturnsNotFound) {
  EXPECT_THAT(database_manager_.GetDatabase("not-exists"),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseManagerTest, DeleteExistingDatabase) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_));
  GOOGLESQL_EXPECT_OK(database_manager_.DeleteDatabase(database_uri_));
  EXPECT_THAT(database_manager_.GetDatabase(database_uri_),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseManagerTest, ListDatabase) {
  std::string instance_uri = "projects/test-p/instances/test-i";
  int num_databases = 5;

  for (int i = 0; i < num_databases; i++) {
    std::string database_uri =
        absl::StrCat(instance_uri, "/databases/database-", i);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Database> database,
                         database_manager_.CreateDatabase(
                             database_uri, empty_schema_operation_));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::shared_ptr<Database>> databases,
                       database_manager_.ListDatabases(instance_uri));
  EXPECT_EQ(databases.size(), num_databases);
  for (int i = 0; i < num_databases; i++) {
    EXPECT_EQ(databases[i]->database_uri(),
              absl::StrCat(instance_uri, "/databases/database-", i));
  }
}

TEST_F(DatabaseManagerTest, ListDatabaseWithSimilarInstanceUri) {
  std::string similar_database_uri = absl::StrCat(
      "projects/test-p/instances/test-instances/databases/database");

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database,
      database_manager_.CreateDatabase(database_uri_, empty_schema_operation_));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      database, database_manager_.CreateDatabase(similar_database_uri,
                                                 empty_schema_operation_));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::shared_ptr<Database>> databases,
                       database_manager_.ListDatabases(
                           "projects/test-p/instances/test-instance"));
  EXPECT_EQ(databases.size(), 1);
  EXPECT_EQ(databases[0]->database_uri(), database_uri_);
}

// Covers openspec change fix-unique-index-restore-isolation, section 3: a
// database that failed to restore from persisted metadata must be visible
// (not silently absent) but must reject data-plane/DDL access.
TEST_F(DatabaseManagerTest, UnavailableDatabaseHasNoReasonWhenNeverMarked) {
  EXPECT_EQ(database_manager_.UnavailableReason(database_uri_), std::nullopt);
}

TEST_F(DatabaseManagerTest, MarkDatabaseUnavailableRecordsReason) {
  database_manager_.MarkDatabaseUnavailable(
      database_uri_, "UNIQUE violation on index EmployeesByEmail");

  EXPECT_THAT(database_manager_.UnavailableReason(database_uri_),
              testing::Optional(testing::HasSubstr(
                  "UNIQUE violation on index EmployeesByEmail")));
}

TEST_F(DatabaseManagerTest, GetDatabaseRejectsUnavailableDatabaseWithReason) {
  database_manager_.MarkDatabaseUnavailable(database_uri_,
                                            "persisted data is corrupted");

  EXPECT_THAT(
      database_manager_.GetDatabase(database_uri_),
      googlesql_base::testing::StatusIs(
          absl::StatusCode::kFailedPrecondition,
          testing::AllOf(testing::HasSubstr(database_uri_),
                         testing::HasSubstr("persisted data is corrupted"),
                         testing::HasSubstr("repair_corrupted_databases"))));
}

TEST_F(DatabaseManagerTest, UnavailableDatabaseDoesNotAffectOtherDatabases) {
  const std::string other_uri =
      "projects/test-p/instances/test-instance/databases/other-database";
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> other,
      database_manager_.CreateDatabase(other_uri, empty_schema_operation_));
  database_manager_.MarkDatabaseUnavailable(database_uri_,
                                            "corrupted unique index");

  EXPECT_THAT(database_manager_.GetDatabase(database_uri_),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kFailedPrecondition));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Database> fetched_other,
                       database_manager_.GetDatabase(other_uri));
  EXPECT_EQ(fetched_other, other);
}

TEST_F(DatabaseManagerTest, UnavailableDatabaseNameIsTakenUntilDeleted) {
  database_manager_.MarkDatabaseUnavailable(database_uri_,
                                            "persisted data is corrupted");
  EXPECT_THAT(database_manager_.ReserveDatabase(database_uri_),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kAlreadyExists));

  GOOGLESQL_ASSERT_OK(database_manager_.DeleteDatabase(database_uri_));
  EXPECT_EQ(database_manager_.UnavailableReason(database_uri_), std::nullopt);
  EXPECT_TRUE(database_manager_
                  .ListUnavailableDatabases(
                      "projects/test-p/instances/test-instance")
                  .empty());
  GOOGLESQL_ASSERT_OK(database_manager_.CreateDatabase(
      database_uri_, empty_schema_operation_));
  GOOGLESQL_EXPECT_OK(database_manager_.GetDatabase(database_uri_));
}

TEST_F(DatabaseManagerTest, ListUnavailableDatabasesScopedToInstance) {
  const std::string instance_uri = "projects/test-p/instances/test-instance";
  const std::string other_instance_uri =
      "projects/test-p/instances/other-instance";
  const std::string in_instance =
      absl::StrCat(instance_uri, "/databases/broken-a");
  const std::string also_in_instance =
      absl::StrCat(instance_uri, "/databases/broken-b");
  const std::string other_instance_db =
      absl::StrCat(other_instance_uri, "/databases/broken-c");

  database_manager_.MarkDatabaseUnavailable(also_in_instance, "reason-b");
  database_manager_.MarkDatabaseUnavailable(in_instance, "reason-a");
  database_manager_.MarkDatabaseUnavailable(other_instance_db, "reason-c");

  std::vector<std::pair<std::string, std::string>> unavailable =
      database_manager_.ListUnavailableDatabases(instance_uri);
  ASSERT_EQ(unavailable.size(), 2);
  // Sorted by URI, matching ListDatabases()'s ordering.
  EXPECT_EQ(unavailable[0].first, in_instance);
  EXPECT_EQ(unavailable[0].second, "reason-a");
  EXPECT_EQ(unavailable[1].first, also_in_instance);
  EXPECT_EQ(unavailable[1].second, "reason-b");
}

TEST_F(DatabaseManagerTest, DatabaseQuotaIsEnforced) {
  std::string database_uri_prefix =
      "projects/test-project/instances/test-instance/databases/test-database-";

  // Create 100 databases.
  for (int i = 1; i <= 100; ++i) {
    std::string database_uri = absl::StrCat(database_uri_prefix, i);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Database> database,
                         database_manager_.CreateDatabase(
                             database_uri, empty_schema_operation_));
  }

  // The next database creation should fail.
  EXPECT_THAT(
      database_manager_.CreateDatabase(absl::StrCat(database_uri_prefix, 101),
                                       empty_schema_operation_),
      googlesql_base::testing::StatusIs(absl::StatusCode::kResourceExhausted));

  // But creating a database in another instance should not fail.
  GOOGLESQL_EXPECT_OK(database_manager_.CreateDatabase(
      absl::StrCat("projects/test-project/instances/test-instance-2/databases/"
                   "test-database-",
                   101),
      empty_schema_operation_));

  // If we clear some quota, we can create a database again.
  GOOGLESQL_EXPECT_OK(
      database_manager_.DeleteDatabase(absl::StrCat(database_uri_prefix, 100)));
  GOOGLESQL_EXPECT_OK(database_manager_.CreateDatabase(
      absl::StrCat(database_uri_prefix, 101), empty_schema_operation_));
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
