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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "google/iam/v1/iam_policy.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/protobuf/field_mask.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "grpcpp/client_context.h"
#include "gtest/gtest.h"
#include "tests/common/test_env.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace iam_api = ::google::iam::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace spanner_api = ::google::spanner::v1;
namespace protobuf_api = ::google::protobuf;

class PersistentDataDirectory {
 public:
  PersistentDataDirectory()
      : previous_(absl::GetFlag(FLAGS_data_dir)),
        path_(
            (std::filesystem::temp_directory_path() /
             ("spanner-backup-handler-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count())))
                .string()) {
    std::filesystem::create_directories(path_);
    absl::SetFlag(&FLAGS_data_dir, path_);
  }
  const std::string& path() const { return path_; }

  ~PersistentDataDirectory() {
    absl::SetFlag(&FLAGS_data_dir, previous_);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

 private:
  std::string previous_;
  std::string path_;
};

class BackupApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    instance_api::CreateInstanceRequest instance_request;
    instance_request.set_parent(project_name_);
    instance_request.set_instance_id("instance");
    instance_request.mutable_instance()->set_config("emulator-config");
    instance_request.mutable_instance()->set_display_name("Instance");
    instance_request.mutable_instance()->set_node_count(1);
    operations_api::Operation operation;
    grpc::ClientContext instance_context;
    grpc::Status instance_status = env_.instance_admin_client()->CreateInstance(
        &instance_context, instance_request, &operation);
    ASSERT_TRUE(instance_status.ok()) << instance_status.error_message();
    ASSERT_TRUE(operation.done());

    database_api::CreateDatabaseRequest database_request;
    database_request.set_parent(instance_name_);
    database_request.set_create_statement("CREATE DATABASE `source`");
    database_request.add_extra_statements(
        "CREATE TABLE TestRows (Id INT64 NOT NULL, Value STRING(MAX)) "
        "PRIMARY KEY (Id)");
    grpc::ClientContext database_context;
    grpc::Status database_status = env_.database_admin_client()->CreateDatabase(
        &database_context, database_request, &operation);
    ASSERT_TRUE(database_status.ok()) << database_status.error_message();
    ASSERT_TRUE(operation.done());
  }

  grpc::Status CreateSession(const std::string& database,
                             spanner_api::Session* session) {
    spanner_api::CreateSessionRequest request;
    request.set_database(database);
    grpc::ClientContext context;
    return env_.spanner_client()->CreateSession(&context, request, session);
  }

  grpc::Status WriteRow(const std::string& database, const std::string& id,
                        const std::string& value) {
    spanner_api::Session session;
    grpc::Status status = CreateSession(database, &session);
    if (!status.ok()) return status;

    spanner_api::CommitRequest request;
    request.set_session(session.name());
    request.mutable_single_use_transaction()->mutable_read_write();
    auto* write = request.add_mutations()->mutable_insert();
    write->set_table("TestRows");
    write->add_columns("Id");
    write->add_columns("Value");
    auto* row = write->add_values();
    row->add_values()->set_string_value(id);
    row->add_values()->set_string_value(value);
    spanner_api::CommitResponse response;
    grpc::ClientContext context;
    return env_.spanner_client()->Commit(&context, request, &response);
  }

  grpc::Status ReadRows(const std::string& database,
                        spanner_api::ResultSet* response) {
    spanner_api::Session session;
    grpc::Status status = CreateSession(database, &session);
    if (!status.ok()) return status;

    spanner_api::ExecuteSqlRequest request;
    request.set_session(session.name());
    request.mutable_transaction()->mutable_single_use()->mutable_read_only();
    request.set_sql("SELECT Id, Value FROM TestRows ORDER BY Id");
    grpc::ClientContext context;
    return env_.spanner_client()->ExecuteSql(&context, request, response);
  }

  grpc::Status CreateInstance(const std::string& project,
                              const std::string& instance_id,
                              const std::string& config) {
    instance_api::CreateInstanceRequest request;
    request.set_parent(project);
    request.set_instance_id(instance_id);
    request.mutable_instance()->set_config(config);
    request.mutable_instance()->set_display_name(instance_id);
    request.mutable_instance()->set_node_count(1);
    operations_api::Operation operation;
    grpc::ClientContext context;
    return env_.instance_admin_client()->CreateInstance(&context, request,
                                                        &operation);
  }

  int64_t ValidExpirationSeconds() const {
    return absl::ToUnixSeconds(env_.server()->env()->clock()->Now() +
                               absl::Hours(24 * 30));
  }

  grpc::Status CreateBackup(const std::string& parent,
                            const std::string& backup_id,
                            const std::string& database,
                            operations_api::Operation* operation) {
    database_api::CreateBackupRequest request;
    request.set_parent(parent);
    request.set_backup_id(backup_id);
    request.mutable_backup()->set_database(database);
    request.mutable_backup()->mutable_expire_time()->set_seconds(
        ValidExpirationSeconds());
    grpc::ClientContext context;
    return env_.database_admin_client()->CreateBackup(&context, request,
                                                       operation);
  }

  PersistentDataDirectory persistent_data_;
  test::TestEnv env_;
  const std::string project_name_ = "projects/backup-handler-test";
  const std::string instance_name_ = project_name_ + "/instances/instance";
  const std::string database_name_ = instance_name_ + "/databases/source";
  const std::string backup_name_ = instance_name_ + "/backups/primary";
};

TEST_F(BackupApiTest, BackupCopyRestoreAndScheduleLifecycle) {
  ASSERT_TRUE(WriteRow(database_name_, "1", "before-backup").ok());
  database_api::CreateBackupRequest create_request;
  create_request.set_parent(instance_name_);
  create_request.set_backup_id("primary");
  create_request.mutable_backup()->set_database(database_name_);
  create_request.mutable_backup()->mutable_expire_time()->set_seconds(
      ValidExpirationSeconds());
  operations_api::Operation operation;
  grpc::ClientContext create_context;
  grpc::Status status = env_.database_admin_client()->CreateBackup(
      &create_context, create_request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_TRUE(operation.done());
  database_api::Backup created;
  ASSERT_TRUE(operation.response().UnpackTo(&created));
  EXPECT_EQ(created.name(), backup_name_);
  EXPECT_EQ(created.state(), database_api::Backup::READY);
  EXPECT_GT(created.size_bytes(), 0);
  EXPECT_EQ(created.create_time().SerializeAsString(),
            created.version_time().SerializeAsString());

  database_api::GetBackupRequest get_request;
  get_request.set_name(backup_name_);
  database_api::Backup fetched;
  grpc::ClientContext get_context;
  status = env_.database_admin_client()->GetBackup(&get_context, get_request,
                                                   &fetched);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(fetched.SerializeAsString(), created.SerializeAsString());

  database_api::ListBackupsRequest list_request;
  list_request.set_parent(instance_name_);
  database_api::ListBackupsResponse listed;
  grpc::ClientContext list_context;
  status = env_.database_admin_client()->ListBackups(&list_context,
                                                     list_request, &listed);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(listed.backups_size(), 1);
  EXPECT_EQ(listed.backups(0).name(), backup_name_);

  database_api::UpdateBackupRequest missing_mask_request;
  missing_mask_request.mutable_backup()->set_name(backup_name_);
  missing_mask_request.mutable_backup()->mutable_expire_time()->set_seconds(
      created.create_time().seconds() + 10 * 24 * 60 * 60);
  database_api::Backup rejected_update;
  grpc::ClientContext missing_mask_context;
  status = env_.database_admin_client()->UpdateBackup(
      &missing_mask_context, missing_mask_request, &rejected_update);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  database_api::Backup unchanged;
  grpc::ClientContext unchanged_context;
  status = env_.database_admin_client()->GetBackup(
      &unchanged_context, get_request, &unchanged);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(unchanged.SerializeAsString(), created.SerializeAsString());

  database_api::UpdateBackupRequest update_request;
  update_request.mutable_backup()->set_name(backup_name_);
  const int64_t updated_expiration =
      created.create_time().seconds() + 14 * 24 * 60 * 60;
  update_request.mutable_backup()->mutable_expire_time()->set_seconds(
      updated_expiration);
  update_request.mutable_update_mask()->add_paths("expire_time");
  database_api::Backup updated;
  grpc::ClientContext update_context;
  status = env_.database_admin_client()->UpdateBackup(&update_context,
                                                      update_request, &updated);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(updated.expire_time().seconds(), updated_expiration);

  database_api::CopyBackupRequest copy_request;
  copy_request.set_parent(instance_name_);
  copy_request.set_backup_id("copy");
  copy_request.set_source_backup(backup_name_);
  copy_request.mutable_expire_time()->set_seconds(
      created.create_time().seconds() + 7 * 24 * 60 * 60);
  grpc::ClientContext copy_context;
  status = env_.database_admin_client()->CopyBackup(&copy_context, copy_request,
                                                    &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup copied;
  ASSERT_TRUE(operation.response().UnpackTo(&copied));
  EXPECT_EQ(copied.name(), instance_name_ + "/backups/copy");
  EXPECT_EQ(copied.database(), created.database());
  EXPECT_EQ(copied.version_time().SerializeAsString(),
            created.version_time().SerializeAsString());
  EXPECT_EQ(copied.size_bytes(), created.size_bytes());
  EXPECT_EQ(copied.database_dialect(), created.database_dialect());

  database_api::RestoreDatabaseRequest restore_request;
  restore_request.set_parent(instance_name_);
  restore_request.set_database_id("../escaped");
  restore_request.set_backup(backup_name_);
  grpc::ClientContext invalid_restore_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &invalid_restore_context, restore_request, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(persistent_data_.path()) / "projects" /
      "backup-handler-test" / "instances" / "instance" / "escaped"));

  restore_request.set_database_id("UPPER");
  grpc::ClientContext noncanonical_restore_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &noncanonical_restore_context, restore_request, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  restore_request.set_database_id("restored");
  grpc::ClientContext restore_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &restore_context, restore_request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Database restored;
  ASSERT_TRUE(operation.response().UnpackTo(&restored));
  EXPECT_EQ(restored.name(), instance_name_ + "/databases/restored");

  spanner_api::ResultSet restored_rows;
  status = ReadRows(restored.name(), &restored_rows);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(restored_rows.rows_size(), 1);
  ASSERT_EQ(restored_rows.rows(0).values_size(), 2);
  EXPECT_EQ(restored_rows.rows(0).values(0).string_value(), "1");
  EXPECT_EQ(restored_rows.rows(0).values(1).string_value(), "before-backup");

  database_api::UpdateDatabaseDdlRequest ddl_request;
  ddl_request.set_database(restored.name());
  ddl_request.add_statements(
      "CREATE TABLE RestoredOnly (Id INT64 NOT NULL) PRIMARY KEY (Id)");
  grpc::ClientContext ddl_context;
  status = env_.database_admin_client()->UpdateDatabaseDdl(
      &ddl_context, ddl_request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(operation.done());

  database_api::CreateBackupScheduleRequest schedule_request;
  schedule_request.set_parent(database_name_);
  schedule_request.mutable_backup_schedule()
      ->mutable_retention_duration()
      ->set_seconds(7 * 24 * 60 * 60);
  schedule_request.set_backup_schedule_id("daily");
  database_api::BackupSchedule schedule;
  grpc::ClientContext schedule_context;
  status = env_.database_admin_client()->CreateBackupSchedule(
      &schedule_context, schedule_request, &schedule);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(schedule.name(), database_name_ + "/backupSchedules/daily");

  database_api::ListBackupSchedulesRequest schedules_request;
  schedules_request.set_parent(database_name_);
  database_api::ListBackupSchedulesResponse schedules;
  grpc::ClientContext schedules_context;
  status = env_.database_admin_client()->ListBackupSchedules(
      &schedules_context, schedules_request, &schedules);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(schedules.backup_schedules_size(), 1);
  EXPECT_EQ(schedules.backup_schedules(0).name(), schedule.name());

  database_api::DeleteBackupScheduleRequest delete_schedule_request;
  delete_schedule_request.set_name(schedule.name());
  protobuf_api::Empty empty;
  grpc::ClientContext delete_schedule_context;
  iam_api::SetIamPolicyRequest set_schedule_policy;
  set_schedule_policy.set_resource(schedule.name());
  set_schedule_policy.mutable_policy()->set_etag("schedule-policy");
  iam_api::Policy schedule_policy;
  grpc::ClientContext set_schedule_policy_context;
  status = env_.database_admin_client()->SetIamPolicy(
      &set_schedule_policy_context, set_schedule_policy, &schedule_policy);
  ASSERT_TRUE(status.ok()) << status.error_message();
  status = env_.database_admin_client()->DeleteBackupSchedule(
      &delete_schedule_context, delete_schedule_request, &empty);
  ASSERT_TRUE(status.ok()) << status.error_message();

  for (const std::string& name :
       {backup_name_, instance_name_ + "/backups/copy"}) {
    database_api::DeleteBackupRequest delete_request;
    delete_request.set_name(name);
    grpc::ClientContext delete_context;
    status = env_.database_admin_client()->DeleteBackup(&delete_context,
                                                        delete_request, &empty);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  EXPECT_FALSE(env_.server()->env()->GetIamPolicy(schedule.name()).has_value());
  EXPECT_FALSE(env_.server()
                   ->env()
                   ->metadata_store()
                   ->GetIamPolicy(schedule.name())
                   .has_value());
}

TEST_F(BackupApiTest, RejectsHistoricalVersionAndExpiredCapture) {
  database_api::CreateBackupRequest historical;
  historical.set_parent(instance_name_);
  historical.set_backup_id("historical");
  historical.mutable_backup()->set_database(database_name_);
  historical.mutable_backup()->mutable_expire_time()->set_seconds(4102444800);
  historical.mutable_backup()->mutable_version_time()->set_seconds(1);
  operations_api::Operation operation;
  grpc::ClientContext historical_context;
  grpc::Status status = env_.database_admin_client()->CreateBackup(
      &historical_context, historical, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(
      status.error_message(),
      "Historical backup version_time is not supported by the emulator");

  database_api::CreateBackupRequest expired;
  expired.set_parent(instance_name_);
  expired.set_backup_id("expired");
  expired.mutable_backup()->set_database(database_name_);
  expired.mutable_backup()->mutable_expire_time()->set_seconds(1);
  grpc::ClientContext expired_context;
  status = env_.database_admin_client()->CreateBackup(
      &expired_context, expired, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Backup expire_time must be after the capture time");
  EXPECT_TRUE(env_.server()->env()->backup_catalog()->AllBackups().empty());
}

TEST_F(BackupApiTest, RequiresSourceDatabaseInParentInstance) {
  ASSERT_TRUE(CreateInstance(project_name_, "other", "emulator-config").ok());
  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(project_name_ + "/instances/other", "misparented",
                   database_name_, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Backup database must be canonical and belong to the parent "
            "instance");
  EXPECT_TRUE(env_.server()->env()->backup_catalog()->AllBackups().empty());
}

TEST_F(BackupApiTest, CopiesAcrossProjectsAndEnforcesExpirationBounds) {
  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(instance_name_, "primary", database_name_, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup source;
  ASSERT_TRUE(operation.response().UnpackTo(&source));

  const std::string destination_project = "projects/backup-copy-target";
  const std::string destination_instance =
      destination_project + "/instances/destination";
  ASSERT_TRUE(
      CreateInstance(destination_project, "destination", "emulator-config")
          .ok());

  database_api::CopyBackupRequest copy;
  copy.set_parent(destination_instance);
  copy.set_source_backup(source.name());
  copy.set_backup_id("too-short");
  *copy.mutable_expire_time() = source.create_time();
  copy.mutable_expire_time()->set_seconds(source.create_time().seconds() +
                                          6 * 60 * 60 - 1);
  grpc::ClientContext short_context;
  status = env_.database_admin_client()->CopyBackup(&short_context, copy,
                                                     &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  copy.set_backup_id("too-long");
  copy.mutable_expire_time()->set_seconds(source.create_time().seconds() +
                                          366 * 24 * 60 * 60 + 1);
  grpc::ClientContext long_context;
  status = env_.database_admin_client()->CopyBackup(&long_context, copy,
                                                     &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  copy.set_backup_id("copied");
  copy.mutable_expire_time()->set_seconds(source.create_time().seconds() +
                                          6 * 60 * 60);
  grpc::ClientContext valid_context;
  status = env_.database_admin_client()->CopyBackup(&valid_context, copy,
                                                     &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup copied;
  ASSERT_TRUE(operation.response().UnpackTo(&copied));
  EXPECT_EQ(copied.name(), destination_instance + "/backups/copied");
  EXPECT_EQ(copied.database(), source.database());
  EXPECT_EQ(copied.version_time().SerializeAsString(),
            source.version_time().SerializeAsString());
  EXPECT_EQ(copied.expire_time().SerializeAsString(),
            copy.expire_time().SerializeAsString());
  EXPECT_EQ(copied.size_bytes(), source.size_bytes());
}

TEST_F(BackupApiTest, RestoreRejectsAnotherProjectAndMismatchedConfig) {
  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(instance_name_, "primary", database_name_, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();

  const std::string other_project = "projects/backup-restore-target";
  ASSERT_TRUE(CreateInstance(other_project, "target", "emulator-config").ok());
  database_api::RestoreDatabaseRequest restore;
  restore.set_parent(other_project + "/instances/target");
  restore.set_database_id("restored");
  restore.set_backup(backup_name_);
  grpc::ClientContext project_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &project_context, restore, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Restored database must be in the same project as the backup");

  instance_api::CreateInstanceConfigRequest config_request;
  config_request.set_parent(project_name_);
  config_request.set_instance_config_id("custom");
  config_request.mutable_instance_config()->set_display_name("Custom");
  grpc::ClientContext config_context;
  status = env_.instance_admin_client()->CreateInstanceConfig(
      &config_context, config_request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  const std::string config_name = project_name_ + "/instanceConfigs/custom";
  ASSERT_TRUE(CreateInstance(project_name_, "custom-target", config_name).ok());

  restore.set_parent(project_name_ + "/instances/custom-target");
  grpc::ClientContext config_restore_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &config_restore_context, restore, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(status.error_message(),
            "Restore destination instance configuration does not match the "
            "source backup configuration");
}

TEST_F(BackupApiTest, BackupCatalogSaveFailureRollsBackSnapshotAndOperation) {
  const std::filesystem::path blocked_temporary_catalog =
      std::filesystem::path(persistent_data_.path()) /
      "backup_catalog.json.tmp";
  std::filesystem::create_directories(blocked_temporary_catalog);
  const std::filesystem::path snapshot_root =
      std::filesystem::path(
          env_.server()->env()->backup_catalog()->SnapshotDirectory(
              backup_name_))
          .parent_path();

  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(instance_name_, "primary", database_name_, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_TRUE(env_.server()->env()->backup_catalog()->AllBackups().empty());
  auto operations =
      env_.server()->env()->operation_manager()->ListOperations(backup_name_);
  ASSERT_TRUE(operations.ok());
  EXPECT_TRUE(operations->empty());
  EXPECT_FALSE(std::filesystem::exists(snapshot_root));
}

TEST_F(BackupApiTest, RestoreMetadataSaveFailureRollsBackDatabase) {
  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(instance_name_, "primary", database_name_, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();

  const std::filesystem::path blocked_temporary_metadata =
      std::filesystem::path(persistent_data_.path()) / "metadata.json.tmp";
  std::filesystem::create_directories(blocked_temporary_metadata);
  const std::string restored_name = instance_name_ + "/databases/rolled-back";
  database_api::RestoreDatabaseRequest restore;
  restore.set_parent(instance_name_);
  restore.set_database_id("rolled-back");
  restore.set_backup(backup_name_);
  grpc::ClientContext context;
  status = env_.database_admin_client()->RestoreDatabase(
      &context, restore, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(env_.server()
                ->env()
                ->database_manager()
                ->GetDatabase(restored_name)
                .status()
                .code(),
            absl::StatusCode::kNotFound);
  const auto instances =
      env_.server()->env()->metadata_store()->instances();
  auto instance = instances.find(instance_name_);
  ASSERT_NE(instance, instances.end());
  EXPECT_FALSE(instance->second.databases.contains("rolled-back"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string restored_storage,
      backend::Database::PersistentStorageDirectory(persistent_data_.path(),
                                                    restored_name));
  const std::filesystem::path restored_root =
      std::filesystem::path(restored_storage).parent_path();
  EXPECT_FALSE(std::filesystem::exists(restored_root));
  EXPECT_FALSE(
      std::filesystem::exists(restored_root.string() + ".restoring"));
  auto restore_operations =
      env_.server()->env()->operation_manager()->ListOperations(restored_name);
  ASSERT_TRUE(restore_operations.ok());
  EXPECT_TRUE(restore_operations->empty());

  std::filesystem::remove_all(blocked_temporary_metadata);
  grpc::ClientContext retry_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &retry_context, restore, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(std::filesystem::exists(restored_root / "storage"));
  EXPECT_FALSE(
      std::filesystem::exists(restored_root / ".restore-in-progress"));
}

TEST_F(BackupApiTest, RestoreOperationSaveFailureRollsBackAndCanRetry) {
  operations_api::Operation operation;
  grpc::Status status =
      CreateBackup(instance_name_, "primary", database_name_, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();

  const std::filesystem::path blocked_temporary_catalog =
      std::filesystem::path(persistent_data_.path()) /
      "backup_catalog.json.tmp";
  std::filesystem::create_directories(blocked_temporary_catalog);
  const std::string restored_name =
      instance_name_ + "/databases/catalog-rollback";
  database_api::RestoreDatabaseRequest restore;
  restore.set_parent(instance_name_);
  restore.set_database_id("catalog-rollback");
  restore.set_backup(backup_name_);
  grpc::ClientContext context;
  status = env_.database_admin_client()->RestoreDatabase(
      &context, restore, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(env_.server()
                ->env()
                ->database_manager()
                ->GetDatabase(restored_name)
                .status()
                .code(),
            absl::StatusCode::kNotFound);
  const auto instances =
      env_.server()->env()->metadata_store()->instances();
  ASSERT_TRUE(instances.contains(instance_name_));
  EXPECT_FALSE(
      instances.at(instance_name_).databases.contains("catalog-rollback"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string restored_storage,
      backend::Database::PersistentStorageDirectory(persistent_data_.path(),
                                                    restored_name));
  const std::filesystem::path restored_root =
      std::filesystem::path(restored_storage).parent_path();
  EXPECT_FALSE(std::filesystem::exists(restored_root));
  auto restore_operations =
      env_.server()->env()->operation_manager()->ListOperations(restored_name);
  ASSERT_TRUE(restore_operations.ok());
  EXPECT_TRUE(restore_operations->empty());

  std::filesystem::remove_all(blocked_temporary_catalog);
  grpc::ClientContext retry_context;
  status = env_.database_admin_client()->RestoreDatabase(
      &retry_context, restore, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(std::filesystem::exists(restored_root / "storage"));
}

TEST_F(BackupApiTest, BackupScheduleUpdateRequiresMask) {
  database_api::CreateBackupScheduleRequest create;
  create.set_parent(database_name_);
  create.set_backup_schedule_id("daily");
  create.mutable_backup_schedule();
  database_api::BackupSchedule created;
  grpc::ClientContext create_context;
  grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
      &create_context, create, &created);
  ASSERT_TRUE(status.ok()) << status.error_message();

  database_api::UpdateBackupScheduleRequest update;
  *update.mutable_backup_schedule() = created;
  update.mutable_backup_schedule()
      ->mutable_retention_duration()
      ->set_seconds(24 * 60 * 60);
  database_api::BackupSchedule rejected;
  grpc::ClientContext update_context;
  status = env_.database_admin_client()->UpdateBackupSchedule(
      &update_context, update, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  database_api::GetBackupScheduleRequest get;
  get.set_name(created.name());
  database_api::BackupSchedule unchanged;
  grpc::ClientContext get_context;
  status = env_.database_admin_client()->GetBackupSchedule(
      &get_context, get, &unchanged);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(unchanged.SerializeAsString(), created.SerializeAsString());
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
