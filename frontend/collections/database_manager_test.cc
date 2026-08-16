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
#include <string>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/match.h"
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
