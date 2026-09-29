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
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "frontend/handlers/backups.h"
#include "frontend/persistence/backup_catalog.h"
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

void SetFullDailySchedule(database_api::BackupSchedule* schedule) {
  schedule->mutable_spec()->mutable_cron_spec()->set_text("0 2 * * *");
  schedule->mutable_retention_duration()->set_seconds(7 * 24 * 60 * 60);
  schedule->mutable_full_backup_spec();
}

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
  SetFullDailySchedule(schedule_request.mutable_backup_schedule());
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

TEST_F(BackupApiTest, RejectsUnavailableVersionTimeAndExpiredCapture) {
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
  EXPECT_EQ(status.error_message().rfind(
                "Backup version_time must not be earlier than the "
                "database's earliest_version_time",
                0),
            0)
      << status.error_message();

  historical.mutable_backup()->mutable_expire_time()->set_seconds(
      ValidExpirationSeconds());
  historical.mutable_backup()->mutable_version_time()->set_seconds(
      ValidExpirationSeconds());
  grpc::ClientContext future_context;
  status = env_.database_admin_client()->CreateBackup(&future_context,
                                                      historical, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Backup version_time must not be in the future");

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

  copy.set_backup_id("too-soon-from-now");
  copy.mutable_expire_time()->set_seconds(source.create_time().seconds() +
                                          6 * 60 * 60);
  grpc::ClientContext too_soon_context;
  status = env_.database_admin_client()->CopyBackup(
      &too_soon_context, copy, &operation);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  copy.set_backup_id("copied");
  copy.mutable_expire_time()->set_seconds(source.create_time().seconds() +
                                          7 * 60 * 60);
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

TEST_F(BackupApiTest, FiltersAndPaginatesNewestBackupsFirst) {
  operations_api::Operation operation;
  for (const std::string& id : {"zeta", "alpha", "middle"}) {
    ASSERT_TRUE(CreateBackup(instance_name_, id, database_name_, &operation)
                    .ok());
  }

  database_api::ListBackupsRequest request;
  request.set_parent(instance_name_);
  request.set_filter("(name:alpha OR name:middle) AND state:ready");
  request.set_page_size(1);
  database_api::ListBackupsResponse first;
  grpc::ClientContext first_context;
  grpc::Status status = env_.database_admin_client()->ListBackups(
      &first_context, request, &first);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(first.backups_size(), 1);
  EXPECT_EQ(first.backups(0).name(), instance_name_ + "/backups/middle");
  ASSERT_FALSE(first.next_page_token().empty());

  request.set_page_token(first.next_page_token());
  database_api::ListBackupsResponse second;
  grpc::ClientContext second_context;
  status = env_.database_admin_client()->ListBackups(
      &second_context, request, &second);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(second.backups_size(), 1);
  EXPECT_EQ(second.backups(0).name(), instance_name_ + "/backups/alpha");
  EXPECT_TRUE(second.next_page_token().empty());

  request.set_filter("name:zeta");
  database_api::ListBackupsResponse rejected;
  grpc::ClientContext mismatch_context;
  status = env_.database_admin_client()->ListBackups(
      &mismatch_context, request, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  request.set_filter("(name:alpha OR name:middle) AND state:ready");
  request.set_page_token(std::string(16 * 1024 + 1, 'A'));
  grpc::ClientContext oversized_context;
  status = env_.database_admin_client()->ListBackups(
      &oversized_context, request, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  request.set_page_token("not-base64");
  grpc::ClientContext malformed_context;
  status = env_.database_admin_client()->ListBackups(
      &malformed_context, request, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  request.clear_page_token();
  request.set_filter("database:source size_bytes > 0 AND "
                     "expire_time > \"2020-01-01T00:00:00Z\" AND "
                     "NOT name:zeta");
  database_api::ListBackupsResponse all;
  grpc::ClientContext all_context;
  status = env_.database_admin_client()->ListBackups(
      &all_context, request, &all);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(all.backups_size(), 1);
  EXPECT_EQ(all.backups(0).name(), instance_name_ + "/backups/middle");

  request.set_filter("unknown:field");
  grpc::ClientContext invalid_context;
  status = env_.database_admin_client()->ListBackups(
      &invalid_context, request, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(BackupApiTest, FilterOrBindsTighterThanAnd) {
  operations_api::Operation operation;
  for (const std::string& id : {"zeta", "alpha", "middle"}) {
    ASSERT_TRUE(CreateBackup(instance_name_, id, database_name_, &operation)
                    .ok());
  }
  const auto list = [this](const std::string& filter,
                           std::vector<std::string>* ids) -> grpc::Status {
    database_api::ListBackupsRequest request;
    request.set_parent(instance_name_);
    request.set_filter(filter);
    database_api::ListBackupsResponse response;
    grpc::ClientContext context;
    grpc::Status status =
        env_.database_admin_client()->ListBackups(&context, request, &response);
    ids->clear();
    for (const auto& backup : response.backups()) {
      ids->push_back(backup.name().substr(backup.name().rfind('/') + 1));
    }
    return status;
  };
  std::vector<std::string> ids;
  // AIP-160: name:zeta AND (state:creating OR state:ready).
  grpc::Status status =
      list("name:zeta AND state:creating OR state:ready", &ids);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(ids, std::vector<std::string>({"zeta"}));
  status = list("NAME:ALPHA OR Name:'Middle' sizeBytes > 0", &ids);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(ids, std::vector<std::string>({"middle", "alpha"}));
  status = list("NOT (name:alpha OR name:middle) AND STATE = \"ready\"", &ids);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(ids, std::vector<std::string>({"zeta"}));

  // The create operations are filtered by the shared operation filter.
  database_api::ListBackupOperationsRequest operations_request;
  operations_request.set_parent(instance_name_);
  operations_request.set_filter("name:backups/alpha/ AND done:true");
  database_api::ListBackupOperationsResponse operations_response;
  grpc::ClientContext operations_context;
  status = env_.database_admin_client()->ListBackupOperations(
      &operations_context, operations_request, &operations_response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(operations_response.operations_size(), 1);
  EXPECT_NE(operations_response.operations(0).name().find("/backups/alpha/"),
            std::string::npos);
}

TEST(BackupApiRestartTest, UpdatedCopiedAndExpiredBackupsSurviveRestart) {
  PersistentDataDirectory data;
  const std::string parent = "projects/backup-restart/instances/instance";
  const std::string database = parent + "/databases/source";
  const std::string original_name = parent + "/backups/original";
  const std::string copy_name = parent + "/backups/copied";
  database_api::Backup original;
  database_api::Backup copied;

  {
    test::TestEnv env;
    instance_api::CreateInstanceRequest create_instance;
    create_instance.set_parent("projects/backup-restart");
    create_instance.set_instance_id("instance");
    create_instance.mutable_instance()->set_config("emulator-config");
    create_instance.mutable_instance()->set_display_name("Instance");
    create_instance.mutable_instance()->set_node_count(1);
    operations_api::Operation operation;
    grpc::ClientContext instance_context;
    ASSERT_TRUE(env.instance_admin_client()
                    ->CreateInstance(&instance_context, create_instance,
                                     &operation)
                    .ok());

    database_api::CreateDatabaseRequest create_database;
    create_database.set_parent(parent);
    create_database.set_create_statement("CREATE DATABASE `source`");
    grpc::ClientContext database_context;
    ASSERT_TRUE(env.database_admin_client()
                    ->CreateDatabase(&database_context, create_database,
                                     &operation)
                    .ok());

    database_api::CreateBackupRequest create;
    create.set_parent(parent);
    create.set_backup_id("original");
    create.mutable_backup()->set_database(database);
    create.mutable_backup()->mutable_expire_time()->set_seconds(
        absl::ToUnixSeconds(env.server()->env()->clock()->Now() +
                            absl::Hours(24 * 30)));
    grpc::ClientContext create_context;
    ASSERT_TRUE(env.database_admin_client()
                    ->CreateBackup(&create_context, create, &operation)
                    .ok());
    ASSERT_TRUE(operation.response().UnpackTo(&original));

    database_api::UpdateBackupRequest update;
    update.mutable_backup()->set_name(original_name);
    update.mutable_backup()->mutable_expire_time()->set_seconds(
        original.create_time().seconds() + 15 * 24 * 60 * 60);
    update.mutable_update_mask()->add_paths("expire_time");
    grpc::ClientContext update_context;
    ASSERT_TRUE(env.database_admin_client()
                    ->UpdateBackup(&update_context, update, &original)
                    .ok());

    database_api::CopyBackupRequest copy;
    copy.set_parent(parent);
    copy.set_backup_id("copied");
    copy.set_source_backup(original_name);
    copy.mutable_expire_time()->set_seconds(
        original.create_time().seconds() + 10 * 24 * 60 * 60);
    grpc::ClientContext copy_context;
    ASSERT_TRUE(env.database_admin_client()
                    ->CopyBackup(&copy_context, copy, &operation)
                    .ok());
    ASSERT_TRUE(operation.response().UnpackTo(&copied));
  }

  {
    test::TestEnv env;
    ASSERT_TRUE(env.server()->env()->backup_catalog()->Load().ok());
    for (const auto& expected : {original, copied}) {
      database_api::GetBackupRequest get;
      get.set_name(expected.name());
      database_api::Backup actual;
      grpc::ClientContext context;
      ASSERT_TRUE(env.database_admin_client()->GetBackup(&context, get, &actual)
                      .ok());
      EXPECT_EQ(actual.SerializeAsString(), expected.SerializeAsString());
    }

    auto* catalog = env.server()->env()->backup_catalog();
    auto expiring = catalog->GetBackup(original_name);
    ASSERT_TRUE(expiring.ok());
    expiring->backup.mutable_expire_time()->set_seconds(1);
    ASSERT_TRUE(catalog->UpdateBackup(expiring->backup).ok());
    database_api::GetBackupRequest get;
    get.set_name(original_name);
    database_api::Backup actual;
    grpc::ClientContext context;
    EXPECT_EQ(env.database_admin_client()
                  ->GetBackup(&context, get, &actual)
                  .error_code(),
              grpc::StatusCode::NOT_FOUND);
    EXPECT_FALSE(std::filesystem::exists(
        std::filesystem::path(catalog->SnapshotDirectory(original_name))
            .parent_path()));
  }

  {
    test::TestEnv env;
    ASSERT_TRUE(env.server()->env()->backup_catalog()->Load().ok());
    database_api::GetBackupRequest get;
    get.set_name(copy_name);
    database_api::Backup response;
    grpc::ClientContext context;
    ASSERT_TRUE(env.database_admin_client()
                    ->GetBackup(&context, get, &response)
                    .ok());
    EXPECT_EQ(response.name(), copy_name);
    EXPECT_EQ(env.server()->env()->backup_catalog()->AllBackups().size(), 1);
  }
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
  config_request.set_instance_config_id("custom-backup-restore");
  const std::string config_name =
      project_name_ + "/instanceConfigs/custom-backup-restore";
  config_request.mutable_instance_config()->set_name(config_name);
  config_request.mutable_instance_config()->set_base_config(
      project_name_ + "/instanceConfigs/emulator-config");
  config_request.mutable_instance_config()->set_display_name("Custom");
  grpc::ClientContext config_context;
  status = env_.instance_admin_client()->CreateInstanceConfig(
      &config_context, config_request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
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
  SetFullDailySchedule(create.mutable_backup_schedule());
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

TEST_F(BackupApiTest, SupportedScheduleFrequenciesAndCalendarValidation) {
  for (const auto& [id, cron] :
       {std::pair{"twelve-hour", "0 2/12 * * *"},
        std::pair{"daily", "13 5 * * *"},
        std::pair{"weekly", "17 8 * * 0"},
        std::pair{"monthly", "23 7 8 * *"}}) {
    database_api::CreateBackupScheduleRequest create;
    create.set_parent(database_name_);
    create.set_backup_schedule_id(id);
    SetFullDailySchedule(create.mutable_backup_schedule());
    create.mutable_backup_schedule()->mutable_spec()->mutable_cron_spec()->
        set_text(cron);
    database_api::BackupSchedule created;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
        &context, create, &created);
    ASSERT_TRUE(status.ok()) << cron << ": " << status.error_message();
  }
  for (const auto& [id, cron] :
       {std::pair{"mixed-days", "0 2 8 * 0"},
        std::pair{"restricted-month", "0 2 8 2 *"}}) {
    database_api::CreateBackupScheduleRequest create;
    create.set_parent(database_name_);
    create.set_backup_schedule_id(id);
    SetFullDailySchedule(create.mutable_backup_schedule());
    create.mutable_backup_schedule()->mutable_spec()->mutable_cron_spec()->
        set_text(cron);
    database_api::BackupSchedule ignored;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
        &context, create, &ignored);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT)
        << cron;
  }
}

TEST_F(BackupApiTest, DueScheduleCreatesOneBackupPerInterval) {
  const absl::Time created_at = env_.server()->env()->clock()->Now();
  database_api::CreateBackupScheduleRequest create;
  create.set_parent(database_name_);
  create.set_backup_schedule_id("daily");
  auto* schedule = create.mutable_backup_schedule();
  SetFullDailySchedule(schedule);
  schedule->mutable_spec()->mutable_cron_spec()->set_text(
      absl::FormatTime("%M %H * * *", created_at, absl::UTCTimeZone()));
  database_api::BackupSchedule created;
  grpc::ClientContext context;
  grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
      &context, create, &created);
  ASSERT_TRUE(status.ok()) << status.error_message();

  const absl::Time due = created_at + absl::Hours(24) + absl::Minutes(2);
  absl::Status run_status = RunDueBackupSchedules(env_.server()->env(), due);
  ASSERT_TRUE(run_status.ok()) << run_status;
  const auto backups =
      env_.server()->env()->backup_catalog()->ListBackups(instance_name_);
  ASSERT_EQ(backups.size(), 1);
  EXPECT_EQ(backups[0].backup.backup_schedules_size(), 1);
  EXPECT_EQ(backups[0].backup.backup_schedules(0), created.name());
  EXPECT_GT(backups[0].backup.size_bytes(), 0);
  EXPECT_NEAR(backups[0].backup.expire_time().seconds() -
                  backups[0].backup.create_time().seconds(),
              7 * 24 * 60 * 60, 2);

  run_status = RunDueBackupSchedules(env_.server()->env(), due);
  ASSERT_TRUE(run_status.ok()) << run_status;
  EXPECT_EQ(env_.server()->env()->backup_catalog()
                ->ListBackups(instance_name_)
                .size(),
            1);

  database_api::CopyBackupRequest copy;
  copy.set_parent(instance_name_);
  copy.set_backup_id("scheduled-copy");
  copy.set_source_backup(backups[0].backup.name());
  copy.mutable_expire_time()->set_seconds(ValidExpirationSeconds());
  operations_api::Operation copied_operation;
  grpc::ClientContext copy_context;
  status = env_.database_admin_client()->CopyBackup(
      &copy_context, copy, &copied_operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup copied;
  ASSERT_TRUE(copied_operation.response().UnpackTo(&copied));
  EXPECT_EQ(copied.backup_schedules_size(), 0);
}

TEST_F(BackupApiTest, DueScheduleUsesCaptureTimeForMaximumRetention) {
  const absl::Time created_at = env_.server()->env()->clock()->Now();
  database_api::CreateBackupScheduleRequest create;
  create.set_parent(database_name_);
  create.set_backup_schedule_id("maximum-retention");
  SetFullDailySchedule(create.mutable_backup_schedule());
  create.mutable_backup_schedule()->mutable_retention_duration()->set_seconds(
      366 * 24 * 60 * 60);
  create.mutable_backup_schedule()
      ->mutable_spec()
      ->mutable_cron_spec()
      ->set_text(
          absl::FormatTime("%M %H * * *", created_at, absl::UTCTimeZone()));
  database_api::BackupSchedule created;
  grpc::ClientContext context;
  grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
      &context, create, &created);
  ASSERT_TRUE(status.ok()) << status.error_message();

  ASSERT_TRUE(RunDueBackupSchedules(env_.server()->env(),
                                     created_at + absl::Hours(24) +
                                         absl::Minutes(2))
                  .ok());
  const auto backups =
      env_.server()->env()->backup_catalog()->ListBackups(instance_name_);
  ASSERT_EQ(backups.size(), 1);
  EXPECT_EQ(backups[0].backup.expire_time().seconds() -
                backups[0].backup.create_time().seconds(),
            366 * 24 * 60 * 60);
  EXPECT_EQ(backups[0].backup.expire_time().nanos(),
            backups[0].backup.create_time().nanos());
}

TEST_F(BackupApiTest, ScheduleChangesRetentionAndCron) {
  const absl::Time now = env_.server()->env()->clock()->Now();
  database_api::CreateBackupScheduleRequest create;
  create.set_parent(database_name_);
  create.set_backup_schedule_id("daily");
  SetFullDailySchedule(create.mutable_backup_schedule());
  create.mutable_backup_schedule()->mutable_spec()->mutable_cron_spec()->
      set_text(absl::FormatTime("%M %H * * *", now,
                                absl::UTCTimeZone()));
  database_api::BackupSchedule created;
  grpc::ClientContext create_context;
  grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
      &create_context, create, &created);
  ASSERT_TRUE(status.ok()) << status.error_message();
  const int64_t original_due = env_.server()
                                   ->env()
                                   ->backup_catalog()
                                   ->AllBackupSchedules()[0]
                                   .next_due_seconds;

  database_api::UpdateBackupScheduleRequest update;
  update.mutable_backup_schedule()->set_name(created.name());
  update.mutable_backup_schedule()
      ->mutable_retention_duration()
      ->set_seconds(24 * 60 * 60);
  update.mutable_update_mask()->add_paths("retention_duration");
  database_api::BackupSchedule updated;
  grpc::ClientContext retention_context;
  status = env_.database_admin_client()->UpdateBackupSchedule(
      &retention_context, update, &updated);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(env_.server()
                ->env()
                ->backup_catalog()
                ->AllBackupSchedules()[0]
                .next_due_seconds,
            original_due);

  update.Clear();
  update.mutable_backup_schedule()->set_name(created.name());
  update.mutable_backup_schedule()->mutable_spec()->mutable_cron_spec()->
      set_text(absl::FormatTime("%M %H * * *", now + absl::Minutes(1),
                                absl::UTCTimeZone()));
  update.mutable_update_mask()->add_paths("spec.cron_spec.text");
  grpc::ClientContext cron_context;
  status = env_.database_admin_client()->UpdateBackupSchedule(
      &cron_context, update, &updated);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_NE(env_.server()
                ->env()
                ->backup_catalog()
                ->AllBackupSchedules()[0]
                .next_due_seconds,
            original_due);
  absl::Status run_status =
      RunDueBackupSchedules(env_.server()->env(), now + absl::Minutes(2));
  ASSERT_TRUE(run_status.ok()) << run_status;
  const auto backups =
      env_.server()->env()->backup_catalog()->ListBackups(instance_name_);
  ASSERT_EQ(backups.size(), 1);
  EXPECT_NEAR(backups[0].backup.expire_time().seconds() -
                  backups[0].backup.create_time().seconds(),
              24 * 60 * 60, 2);

  BackupCatalog reloaded(persistent_data_.path());
  ASSERT_TRUE(reloaded.Load().ok());
  ASSERT_EQ(reloaded.AllBackupSchedules().size(), 1);
  EXPECT_EQ(reloaded.AllBackupSchedules()[0].next_due_seconds,
            env_.server()
                ->env()
                ->backup_catalog()
                ->AllBackupSchedules()[0]
                .next_due_seconds);
  EXPECT_EQ(reloaded.ListBackups(instance_name_).size(), 1);

  database_api::Backup expired = backups[0].backup;
  expired.mutable_expire_time()->set_seconds(
      absl::ToUnixSeconds(env_.server()->env()->clock()->Now()) - 1);
  ASSERT_TRUE(
      env_.server()->env()->backup_catalog()->UpdateBackup(expired).ok());
  database_api::ListBackupsRequest list;
  list.set_parent(instance_name_);
  database_api::ListBackupsResponse listed;
  grpc::ClientContext list_context;
  status = env_.database_admin_client()->ListBackups(&list_context, list,
                                                     &listed);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(listed.backups_size(), 0);
  EXPECT_EQ(env_.server()->env()->backup_catalog()->AllBackupSchedules().size(),
            1);

  database_api::DeleteBackupScheduleRequest remove;
  remove.set_name(created.name());
  protobuf_api::Empty ignored;
  grpc::ClientContext remove_context;
  status = env_.database_admin_client()->DeleteBackupSchedule(
      &remove_context, remove, &ignored);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(env_.server()->env()->backup_catalog()->AllBackupSchedules()
                  .empty());
}

TEST_F(BackupApiTest, SchedulePaginationAndUnsupportedEncryption) {
  for (const char* id : {"alpha", "bravo", "charlie"}) {
    database_api::CreateBackupScheduleRequest create;
    create.set_parent(database_name_);
    create.set_backup_schedule_id(id);
    SetFullDailySchedule(create.mutable_backup_schedule());
    database_api::BackupSchedule created;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
        &context, create, &created);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  database_api::ListBackupSchedulesRequest list;
  list.set_parent(database_name_);
  list.set_page_size(1);
  for (const char* id : {"alpha", "bravo", "charlie"}) {
    database_api::ListBackupSchedulesResponse page;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->ListBackupSchedules(
        &context, list, &page);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(page.backup_schedules_size(), 1);
    EXPECT_EQ(page.backup_schedules(0).name(),
              database_name_ + "/backupSchedules/" + id);
    list.set_page_token(page.next_page_token());
  }
  EXPECT_TRUE(list.page_token().empty());
  list.set_page_token("not-a-valid-token");
  database_api::ListBackupSchedulesResponse rejected;
  grpc::ClientContext bad_token_context;
  grpc::Status status = env_.database_admin_client()->ListBackupSchedules(
      &bad_token_context, list, &rejected);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  database_api::CreateBackupScheduleRequest encrypted;
  encrypted.set_parent(database_name_);
  encrypted.set_backup_schedule_id("encrypted");
  SetFullDailySchedule(encrypted.mutable_backup_schedule());
  encrypted.mutable_backup_schedule()
      ->mutable_encryption_config()
      ->set_encryption_type(
          database_api::CreateBackupEncryptionConfig::
              CUSTOMER_MANAGED_ENCRYPTION);
  encrypted.mutable_backup_schedule()
      ->mutable_encryption_config()
      ->set_kms_key_name("projects/p/locations/l/keyRings/r/cryptoKeys/k");
  database_api::BackupSchedule ignored;
  grpc::ClientContext encryption_context;
  status = env_.database_admin_client()->CreateBackupSchedule(
      &encryption_context, encrypted, &ignored);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

spanner_api::Mutation UpsertRow(const std::string& table,
                                const std::vector<std::string>& columns,
                                const std::vector<std::string>& values) {
  spanner_api::Mutation mutation;
  auto* write = mutation.mutable_insert_or_update();
  write->set_table(table);
  for (const std::string& column : columns) write->add_columns(column);
  auto* row = write->add_values();
  for (const std::string& value : values) {
    row->add_values()->set_string_value(value);
  }
  return mutation;
}

grpc::Status CommitMutation(test::TestEnv* env, const std::string& database,
                    const spanner_api::Mutation& mutation) {
  spanner_api::CreateSessionRequest session_request;
  session_request.set_database(database);
  spanner_api::Session session;
  grpc::ClientContext session_context;
  grpc::Status status = env->spanner_client()->CreateSession(
      &session_context, session_request, &session);
  if (!status.ok()) return status;
  spanner_api::CommitRequest request;
  request.set_session(session.name());
  request.mutable_single_use_transaction()->mutable_read_write();
  *request.add_mutations() = mutation;
  spanner_api::CommitResponse response;
  grpc::ClientContext context;
  return env->spanner_client()->Commit(&context, request, &response);
}

// Returns each row of the query as its comma-separated values.
grpc::Status QueryRows(test::TestEnv* env, const std::string& database,
                   const std::string& sql, std::vector<std::string>* rows) {
  spanner_api::CreateSessionRequest session_request;
  session_request.set_database(database);
  spanner_api::Session session;
  grpc::ClientContext session_context;
  grpc::Status status = env->spanner_client()->CreateSession(
      &session_context, session_request, &session);
  if (!status.ok()) return status;
  spanner_api::ExecuteSqlRequest request;
  request.set_session(session.name());
  request.mutable_transaction()->mutable_single_use()->mutable_read_only();
  request.set_sql(sql);
  spanner_api::ResultSet result;
  grpc::ClientContext context;
  status = env->spanner_client()->ExecuteSql(&context, request, &result);
  rows->clear();
  for (const auto& row : result.rows()) {
    std::string text;
    for (const auto& value : row.values()) {
      if (!text.empty()) text += ",";
      text += value.string_value();
    }
    rows->push_back(text);
  }
  return status;
}

grpc::Status ApplyDdl(test::TestEnv* env, const std::string& database,
                       const std::vector<std::string>& statements) {
  database_api::UpdateDatabaseDdlRequest request;
  request.set_database(database);
  for (const std::string& statement : statements) {
    request.add_statements(statement);
  }
  operations_api::Operation operation;
  grpc::ClientContext context;
  grpc::Status status = env->database_admin_client()->UpdateDatabaseDdl(
      &context, request, &operation);
  if (status.ok() && operation.has_error()) {
    return grpc::Status(
        static_cast<grpc::StatusCode>(operation.error().code()),
        operation.error().message());
  }
  return status;
}

// Returns the database's DDL statements joined by newlines.
grpc::Status GetDdl(test::TestEnv* env, const std::string& database,
                    std::string* ddl) {
  database_api::GetDatabaseDdlRequest request;
  request.set_database(database);
  database_api::GetDatabaseDdlResponse response;
  grpc::ClientContext context;
  grpc::Status status =
      env->database_admin_client()->GetDatabaseDdl(&context, request,
                                                   &response);
  ddl->clear();
  for (const std::string& statement : response.statements()) {
    *ddl += statement + "\n";
  }
  return status;
}

grpc::Status RestoreBackup(test::TestEnv* env, const std::string& parent,
                     const std::string& database_id, const std::string& backup,
                     database_api::Database* restored) {
  database_api::RestoreDatabaseRequest request;
  request.set_parent(parent);
  request.set_database_id(database_id);
  request.set_backup(backup);
  operations_api::Operation operation;
  grpc::ClientContext context;
  grpc::Status status = env->database_admin_client()->RestoreDatabase(
      &context, request, &operation);
  if (status.ok()) operation.response().UnpackTo(restored);
  return status;
}

TEST_F(BackupApiTest, VersionTimeBackupHoldsHistoricalDataAndSchema) {
  ASSERT_TRUE(ApplyDdl(&env_, database_name_,
                        {"CREATE TABLE Dropped (Id INT64 NOT NULL) "
                         "PRIMARY KEY (Id)"})
                  .ok());
  ASSERT_TRUE(WriteRow(database_name_, "1", "at-version-time").ok());
  ASSERT_TRUE(CommitMutation(&env_, database_name_,
                             UpsertRow("Dropped", {"Id"}, {"7"}))
                  .ok());
  const absl::Time version_time = env_.server()->env()->clock()->Now();
  ASSERT_TRUE(CommitMutation(&env_, database_name_,
                     UpsertRow("TestRows", {"Id", "Value"}, {"1", "later"}))
                  .ok());
  ASSERT_TRUE(WriteRow(database_name_, "2", "inserted-later").ok());
  // Dropping the table after version_time keeps its rows for the backup, and
  // the later table is not part of the backup's schema.
  ASSERT_TRUE(ApplyDdl(&env_, database_name_,
                        {"CREATE TABLE Later (Id INT64 NOT NULL) "
                         "PRIMARY KEY (Id)",
                         "DROP TABLE Dropped"})
                  .ok());

  database_api::CreateBackupRequest request;
  request.set_parent(instance_name_);
  request.set_backup_id("historical");
  request.mutable_backup()->set_database(database_name_);
  request.mutable_backup()->mutable_expire_time()->set_seconds(
      ValidExpirationSeconds());
  request.mutable_backup()->mutable_version_time()->set_seconds(
      absl::ToUnixSeconds(version_time));
  request.mutable_backup()->mutable_version_time()->set_nanos(
      absl::ToInt64Nanoseconds(version_time -
                               absl::FromUnixSeconds(
                                   absl::ToUnixSeconds(version_time))));
  operations_api::Operation operation;
  grpc::ClientContext context;
  grpc::Status status =
      env_.database_admin_client()->CreateBackup(&context, request, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup backup;
  ASSERT_TRUE(operation.response().UnpackTo(&backup));
  EXPECT_EQ(backup.version_time().SerializeAsString(),
            request.backup().version_time().SerializeAsString());
  EXPECT_GT(backup.create_time().seconds() * 1000000000LL +
                backup.create_time().nanos(),
            backup.version_time().seconds() * 1000000000LL +
                backup.version_time().nanos());
  EXPECT_EQ(backup.oldest_version_time().SerializeAsString(),
            backup.version_time().SerializeAsString());
  EXPECT_EQ(backup.encryption_info().encryption_type(),
            database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);
  EXPECT_EQ(backup.freeable_size_bytes(), backup.size_bytes());
  EXPECT_EQ(backup.exclusive_size_bytes(), backup.size_bytes());
  EXPECT_TRUE(backup.incremental_backup_chain_id().empty());

  database_api::Database restored;
  status = RestoreBackup(&env_, instance_name_, "historical", backup.name(),
                         &restored);
  ASSERT_TRUE(status.ok()) << status.error_message();
  std::vector<std::string> rows;
  status = QueryRows(&env_, restored.name(),
                 "SELECT Id, Value FROM TestRows ORDER BY Id", &rows);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(rows, std::vector<std::string>({"1,at-version-time"}));
  status = QueryRows(&env_, restored.name(), "SELECT Id FROM Dropped", &rows);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(rows, std::vector<std::string>({"7"}));
  status = QueryRows(&env_, restored.name(), "SELECT Id FROM Later", &rows);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(BackupApiTest, EncryptionConfigReportsGoogleDefaultAndRejectsCmek) {
  const std::string kms_key = "projects/p/locations/l/keyRings/r/cryptoKeys/k";
  const auto create = [this](const std::string& id,
                             const database_api::CreateBackupEncryptionConfig&
                                 encryption,
                             database_api::Backup* backup) {
    database_api::CreateBackupRequest request;
    request.set_parent(instance_name_);
    request.set_backup_id(id);
    request.mutable_backup()->set_database(database_name_);
    request.mutable_backup()->mutable_expire_time()->set_seconds(
        ValidExpirationSeconds());
    *request.mutable_encryption_config() = encryption;
    operations_api::Operation operation;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->CreateBackup(
        &context, request, &operation);
    if (status.ok()) operation.response().UnpackTo(backup);
    return status;
  };
  database_api::CreateBackupEncryptionConfig encryption;
  database_api::Backup backup;
  for (const auto type :
       {database_api::CreateBackupEncryptionConfig::USE_DATABASE_ENCRYPTION,
        database_api::CreateBackupEncryptionConfig::
            GOOGLE_DEFAULT_ENCRYPTION}) {
    encryption.set_encryption_type(type);
    const std::string id =
        type == database_api::CreateBackupEncryptionConfig::
                    USE_DATABASE_ENCRYPTION
            ? "database-encryption"
            : "google-default";
    grpc::Status status = create(id, encryption, &backup);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(backup.encryption_info().encryption_type(),
              database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);
  }

  encryption.set_kms_key_name(kms_key);
  EXPECT_EQ(create("key-without-cmek", encryption, &backup).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  encryption.Clear();
  EXPECT_EQ(create("unspecified", encryption, &backup).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  encryption.set_encryption_type(
      database_api::CreateBackupEncryptionConfig::CUSTOMER_MANAGED_ENCRYPTION);
  EXPECT_EQ(create("cmek-without-key", encryption, &backup).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  encryption.add_kms_key_names(kms_key);
  EXPECT_EQ(create("cmek", encryption, &backup).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);

  database_api::CopyBackupRequest copy;
  copy.set_parent(instance_name_);
  copy.set_source_backup(instance_name_ + "/backups/google-default");
  copy.set_backup_id("copy-cmek");
  copy.mutable_expire_time()->set_seconds(ValidExpirationSeconds());
  copy.mutable_encryption_config()->set_encryption_type(
      database_api::CopyBackupEncryptionConfig::CUSTOMER_MANAGED_ENCRYPTION);
  copy.mutable_encryption_config()->set_kms_key_name(kms_key);
  operations_api::Operation operation;
  grpc::ClientContext cmek_copy_context;
  EXPECT_EQ(env_.database_admin_client()
                ->CopyBackup(&cmek_copy_context, copy, &operation)
                .error_code(),
            grpc::StatusCode::UNIMPLEMENTED);
  copy.set_backup_id("copy-default");
  copy.mutable_encryption_config()->set_encryption_type(
      database_api::CopyBackupEncryptionConfig::
          USE_CONFIG_DEFAULT_OR_BACKUP_ENCRYPTION);
  copy.mutable_encryption_config()->clear_kms_key_name();
  grpc::ClientContext copy_context;
  grpc::Status status = env_.database_admin_client()->CopyBackup(
      &copy_context, copy, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  database_api::Backup copied;
  ASSERT_TRUE(operation.response().UnpackTo(&copied));
  EXPECT_EQ(copied.encryption_info().encryption_type(),
            database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);

  database_api::RestoreDatabaseRequest restore;
  restore.set_parent(instance_name_);
  restore.set_database_id("restored-cmek");
  restore.set_backup(copied.name());
  restore.mutable_encryption_config()->set_encryption_type(
      database_api::RestoreDatabaseEncryptionConfig::
          CUSTOMER_MANAGED_ENCRYPTION);
  restore.mutable_encryption_config()->set_kms_key_name(kms_key);
  grpc::ClientContext cmek_restore_context;
  EXPECT_EQ(env_.database_admin_client()
                ->RestoreDatabase(&cmek_restore_context, restore, &operation)
                .error_code(),
            grpc::StatusCode::UNIMPLEMENTED);
  restore.set_database_id("restored-default");
  restore.mutable_encryption_config()->set_encryption_type(
      database_api::RestoreDatabaseEncryptionConfig::GOOGLE_DEFAULT_ENCRYPTION);
  restore.mutable_encryption_config()->clear_kms_key_name();
  grpc::ClientContext restore_context;
  status = env_.database_admin_client()->RestoreDatabase(&restore_context,
                                                         restore, &operation);
  EXPECT_TRUE(status.ok()) << status.error_message();

  database_api::CreateBackupScheduleRequest schedule;
  schedule.set_parent(database_name_);
  schedule.set_backup_schedule_id("google-default");
  SetFullDailySchedule(schedule.mutable_backup_schedule());
  schedule.mutable_backup_schedule()
      ->mutable_encryption_config()
      ->set_encryption_type(database_api::CreateBackupEncryptionConfig::
                                GOOGLE_DEFAULT_ENCRYPTION);
  database_api::BackupSchedule created;
  grpc::ClientContext schedule_context;
  status = env_.database_admin_client()->CreateBackupSchedule(
      &schedule_context, schedule, &created);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(created.encryption_config().encryption_type(),
            database_api::CreateBackupEncryptionConfig::
                GOOGLE_DEFAULT_ENCRYPTION);
  schedule.set_backup_schedule_id("key-without-cmek");
  schedule.mutable_backup_schedule()
      ->mutable_encryption_config()
      ->set_kms_key_name(kms_key);
  grpc::ClientContext rejected_schedule_context;
  EXPECT_EQ(env_.database_admin_client()
                ->CreateBackupSchedule(&rejected_schedule_context, schedule,
                                       &created)
                .error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(BackupApiTest, DatabaseHasAtMostFourBackupSchedules) {
  for (const std::string id : {"one", "two", "three", "four", "five"}) {
    database_api::CreateBackupScheduleRequest create;
    create.set_parent(database_name_);
    create.set_backup_schedule_id(id);
    SetFullDailySchedule(create.mutable_backup_schedule());
    database_api::BackupSchedule created;
    grpc::ClientContext context;
    grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
        &context, create, &created);
    if (id == "five") {
      EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    } else {
      EXPECT_TRUE(status.ok()) << id << ": " << status.error_message();
    }
  }
}

TEST_F(BackupApiTest, IncrementalScheduleBuildsBackupChains) {
  BackupCatalog* catalog = env_.server()->env()->backup_catalog();
  database_api::CreateBackupScheduleRequest create;
  create.set_parent(database_name_);
  create.set_backup_schedule_id("incremental");
  database_api::BackupSchedule* requested = create.mutable_backup_schedule();
  requested->mutable_spec()->mutable_cron_spec()->set_text("0 */3 * * *");
  requested->mutable_incremental_backup_spec();
  database_api::BackupSchedule schedule;
  grpc::ClientContext too_frequent_context;
  grpc::Status status = env_.database_admin_client()->CreateBackupSchedule(
      &too_frequent_context, create, &schedule);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Incremental backup schedules must be at least 4 hours apart");
  requested->mutable_spec()->mutable_cron_spec()->set_text("0 */4 * * *");
  requested->mutable_full_backup_spec();
  grpc::ClientContext full_context;
  status = env_.database_admin_client()->CreateBackupSchedule(
      &full_context, create, &schedule);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(status.error_message(),
            "Full backup schedules must be at least 12 hours apart");
  requested->mutable_incremental_backup_spec();
  grpc::ClientContext create_context;
  status = env_.database_admin_client()->CreateBackupSchedule(
      &create_context, create, &schedule);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(schedule.has_incremental_backup_spec());

  // Runs the schedule at its next due time and returns the backup it made.
  const auto run_once = [&](database_api::Backup* backup) {
    int64_t due_seconds = 0;
    for (const auto& entry : catalog->AllBackupSchedules()) {
      if (entry.schedule.name() == schedule.name()) {
        due_seconds = entry.next_due_seconds;
      }
    }
    ASSERT_GT(due_seconds, 0);
    absl::Status run_status = RunDueBackupSchedules(
        env_.server()->env(),
        absl::FromUnixSeconds(due_seconds) + absl::Minutes(2));
    ASSERT_TRUE(run_status.ok()) << run_status;
    const std::string suffix = "-" + std::to_string(due_seconds);
    for (const auto& entry : catalog->ListBackups(instance_name_)) {
      const std::string& name = entry.backup.name();
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
              0) {
        *backup = entry.backup;
        return;
      }
    }
    FAIL() << "No backup for due time " << due_seconds;
  };
  const auto get = [&](const std::string& name) {
    auto entry = catalog->GetBackup(name);
    EXPECT_TRUE(entry.ok()) << entry.status();
    return entry.ok() ? entry->backup : database_api::Backup();
  };

  database_api::Backup full;
  ASSERT_NO_FATAL_FAILURE(run_once(&full));
  EXPECT_EQ(full.incremental_backup_chain_id(),
            full.name().substr(full.name().rfind('/') + 1));
  EXPECT_EQ(full.oldest_version_time().SerializeAsString(),
            full.version_time().SerializeAsString());
  EXPECT_EQ(full.exclusive_size_bytes(), full.size_bytes());
  EXPECT_EQ(full.freeable_size_bytes(), full.size_bytes());
  EXPECT_EQ(full.encryption_info().encryption_type(),
            database_api::EncryptionInfo::GOOGLE_DEFAULT_ENCRYPTION);

  ASSERT_TRUE(WriteRow(database_name_, "1", "after-full-backup").ok());
  database_api::Backup incremental;
  ASSERT_NO_FATAL_FAILURE(run_once(&incremental));
  EXPECT_EQ(incremental.incremental_backup_chain_id(),
            full.incremental_backup_chain_id());
  EXPECT_EQ(incremental.oldest_version_time().SerializeAsString(),
            full.version_time().SerializeAsString());
  EXPECT_GT(incremental.exclusive_size_bytes(), 0);
  EXPECT_LT(incremental.exclusive_size_bytes(), incremental.size_bytes());
  EXPECT_EQ(incremental.freeable_size_bytes(),
            incremental.exclusive_size_bytes());
  // The incremental backup needs the full backup's data.
  EXPECT_EQ(get(full.name()).freeable_size_bytes(), 0);

  // Each snapshot is complete, so an incremental backup restores alone.
  database_api::Database restored;
  status = RestoreBackup(&env_, instance_name_, "from-incremental",
                   incremental.name(), &restored);
  ASSERT_TRUE(status.ok()) << status.error_message();
  std::vector<std::string> rows;
  status = QueryRows(&env_, restored.name(),
                 "SELECT Id, Value FROM TestRows ORDER BY Id", &rows);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(rows, std::vector<std::string>({"1,after-full-backup"}));

  // Deleting the chain's newest backup frees the older backup and makes the
  // next backup start a new chain.
  database_api::DeleteBackupRequest remove;
  remove.set_name(incremental.name());
  protobuf_api::Empty empty;
  grpc::ClientContext remove_context;
  status =
      env_.database_admin_client()->DeleteBackup(&remove_context, remove,
                                                 &empty);
  ASSERT_TRUE(status.ok()) << status.error_message();
  const database_api::Backup remaining = get(full.name());
  EXPECT_EQ(remaining.exclusive_size_bytes(), full.size_bytes());
  EXPECT_EQ(remaining.freeable_size_bytes(), full.size_bytes());

  database_api::Backup chain_start;
  ASSERT_NO_FATAL_FAILURE(run_once(&chain_start));
  EXPECT_NE(chain_start.incremental_backup_chain_id(),
            full.incremental_backup_chain_id());
  EXPECT_EQ(chain_start.oldest_version_time().SerializeAsString(),
            chain_start.version_time().SerializeAsString());
  for (int i = 0; i < 13; ++i) {
    database_api::Backup next;
    ASSERT_NO_FATAL_FAILURE(run_once(&next));
    EXPECT_EQ(next.incremental_backup_chain_id(),
              chain_start.incremental_backup_chain_id());
    EXPECT_EQ(next.oldest_version_time().SerializeAsString(),
              chain_start.version_time().SerializeAsString());
  }
  // A chain holds a full backup and at most 13 incremental backups.
  database_api::Backup next_chain;
  ASSERT_NO_FATAL_FAILURE(run_once(&next_chain));
  EXPECT_NE(next_chain.incremental_backup_chain_id(),
            chain_start.incremental_backup_chain_id());
  EXPECT_EQ(get(chain_start.name()).freeable_size_bytes(), 0);

  // The chain state survives a restart.
  BackupCatalog reloaded(persistent_data_.path());
  ASSERT_TRUE(reloaded.Load().ok());
  std::optional<BackupCatalog::BackupChain> chain =
      reloaded.GetBackupChain(schedule.name());
  ASSERT_TRUE(chain.has_value());
  EXPECT_EQ(chain->id, next_chain.incremental_backup_chain_id());
  EXPECT_EQ(chain->backup_count, 1);
  EXPECT_EQ(chain->newest_backup, next_chain.name());
  absl::StatusOr<BackupCatalog::BackupEntry> reloaded_backup =
      reloaded.GetBackup(next_chain.name());
  ASSERT_TRUE(reloaded_backup.ok()) << reloaded_backup.status();
  EXPECT_EQ(reloaded_backup->backup.SerializeAsString(),
            next_chain.SerializeAsString());

  // Modifying the schedule starts a new chain.
  database_api::UpdateBackupScheduleRequest update;
  update.mutable_backup_schedule()->set_name(schedule.name());
  update.mutable_backup_schedule()->mutable_retention_duration()->set_seconds(
      24 * 60 * 60);
  update.mutable_update_mask()->add_paths("retention_duration");
  database_api::BackupSchedule updated;
  grpc::ClientContext update_context;
  status = env_.database_admin_client()->UpdateBackupSchedule(
      &update_context, update, &updated);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(catalog->GetBackupChain(schedule.name()).has_value());
}

TEST_F(BackupApiTest, RestoreDropsRowDeletionPolicies) {
  ASSERT_TRUE(ApplyDdl(&env_, database_name_,
                        {"CREATE TABLE Expiring (Id INT64 NOT NULL, "
                         "CreatedAt TIMESTAMP) PRIMARY KEY (Id), "
                         "ROW DELETION POLICY (OLDER_THAN(CreatedAt, "
                         "INTERVAL 1 DAY))"})
                  .ok());
  ASSERT_TRUE(CommitMutation(&env_, database_name_,
                     UpsertRow("Expiring", {"Id", "CreatedAt"},
                               {"1", "2000-01-01T00:00:00Z"}))
                  .ok());

  database_api::CreateDatabaseRequest create_pg;
  create_pg.set_parent(instance_name_);
  create_pg.set_create_statement("CREATE DATABASE \"pgsource\"");
  create_pg.set_database_dialect(database_api::DatabaseDialect::POSTGRESQL);
  create_pg.add_extra_statements(
      "CREATE TABLE expiring (id bigint PRIMARY KEY, created_at timestamptz) "
      "TTL INTERVAL '1 days' ON created_at");
  operations_api::Operation operation;
  grpc::ClientContext create_pg_context;
  grpc::Status status = env_.database_admin_client()->CreateDatabase(
      &create_pg_context, create_pg, &operation);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_FALSE(operation.has_error()) << operation.error().message();

  using TtlCase = std::tuple<std::string, std::string, std::string,
                             std::string>;
  for (const auto& [source, backup_id, restored_id, policy] :
       std::vector<TtlCase>{
           {database_name_, "gsql-ttl", "gsql-restored",
            "ROW DELETION POLICY"},
           {instance_name_ + "/databases/pgsource", "pg-ttl", "pg-restored",
            "TTL INTERVAL"}}) {
    status = CreateBackup(instance_name_, backup_id, source, &operation);
    ASSERT_TRUE(status.ok()) << status.error_message();
    database_api::Database restored;
    status = RestoreBackup(&env_, instance_name_, restored_id,
                     instance_name_ + "/backups/" + backup_id, &restored);
    ASSERT_TRUE(status.ok()) << status.error_message();

    std::string ddl;
    ASSERT_TRUE(GetDdl(&env_, source, &ddl).ok());
    EXPECT_NE(ddl.find(policy), std::string::npos) << ddl;
    ASSERT_TRUE(GetDdl(&env_, restored.name(), &ddl).ok());
    EXPECT_EQ(ddl.find(policy), std::string::npos) << ddl;

    // The drop is persisted as the restored database's last schema change,
    // so a restart replays it.
    const auto instances =
        env_.server()->env()->metadata_store()->instances();
    const auto& batches = instances.at(instance_name_)
                              .databases.at(restored_id)
                              .schema_change_batches;
    ASSERT_FALSE(batches.empty());
    EXPECT_EQ(batches.back().statements,
              std::vector<std::string>(
                  {restored_id == "pg-restored"
                       ? "ALTER TABLE \"expiring\" DROP TTL"
                       : "ALTER TABLE Expiring DROP ROW DELETION POLICY"}));
    EXPECT_FALSE(batches.back().schema_change_timestamp.empty());
  }
  std::vector<std::string> rows;
  status = QueryRows(&env_, instance_name_ + "/databases/gsql-restored",
                 "SELECT Id FROM Expiring", &rows);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(rows, std::vector<std::string>({"1"}));
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
