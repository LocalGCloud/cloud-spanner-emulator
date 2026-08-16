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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "gmock/gmock.h"
#include "google/iam/v1/iam_policy.pb.h"
#include "google/iam/v1/policy.pb.h"
#include "google/longrunning/operations.pb.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test_env.h"
#include "frontend/persistence/metadata_store.h"

ABSL_DECLARE_FLAG(std::string, data_dir);

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

namespace iam_api = ::google::iam::v1;
namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;


class PolicyPersistentDataDirectory {
 public:
  PolicyPersistentDataDirectory()
      : previous_(absl::GetFlag(FLAGS_data_dir)),
        path_((std::filesystem::temp_directory_path() /
               ("spanner-policy-handler-" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())))
                  .string()) {
    std::filesystem::create_directories(path_);
    absl::SetFlag(&FLAGS_data_dir, path_);
  }

  ~PolicyPersistentDataDirectory() {
    absl::SetFlag(&FLAGS_data_dir, previous_);
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::string& path() const { return path_; }

 private:
  std::string previous_;
  std::string path_;
};
class PolicyApiTest : public test::ServerTest {
 protected:
  void SetUp() override;
};

void CreatePolicyResource(test::TestEnv* env, const std::string& project,
                          const std::string& instance_id,
                          const std::string& database_id = "") {
  instance_api::CreateInstanceRequest create_instance;
  create_instance.set_parent(project);
  create_instance.set_instance_id(instance_id);
  create_instance.mutable_instance()->set_config("emulator-config");
  create_instance.mutable_instance()->set_node_count(1);
  operations_api::Operation operation;
  grpc::ClientContext create_instance_context;
  ASSERT_TRUE(env->instance_admin_client()
                  ->CreateInstance(&create_instance_context, create_instance,
                                   &operation)
                  .ok());
  if (database_id.empty()) return;

  database_api::CreateDatabaseRequest create_database;
  create_database.set_parent(project + "/instances/" + instance_id);
  create_database.set_create_statement("CREATE DATABASE `" + database_id +
                                       "`");
  grpc::ClientContext create_database_context;
  ASSERT_TRUE(env->database_admin_client()
                  ->CreateDatabase(&create_database_context, create_database,
                                   &operation)
                  .ok());
}

void PolicyApiTest::SetUp() {
  test::ServerTest::SetUp();
  CreatePolicyResource(test_env(), "projects/test-project", "test-instance",
                       "test-database");
}

TEST_F(PolicyApiTest, InstancePoliciesRoundTripAndAllowEveryPermission) {
  const std::string resource = "projects/test-project/instances/test-instance";
  iam_api::SetIamPolicyRequest set_request;
  set_request.set_resource(resource);
  set_request.mutable_policy()->set_version(3);
  auto* binding = set_request.mutable_policy()->add_bindings();
  binding->set_role("roles/spanner.databaseReader");
  binding->add_members("user:developer@example.com");

  iam_api::Policy set_response;
  grpc::ClientContext set_context;
  grpc::Status set_status = test_env()->instance_admin_client()->SetIamPolicy(
      &set_context, set_request, &set_response);
  ASSERT_TRUE(set_status.ok()) << set_status.error_message();
  EXPECT_FALSE(set_response.etag().empty());
  EXPECT_EQ(set_response.version(), 3);
  ASSERT_EQ(set_response.bindings_size(), 1);
  EXPECT_EQ(set_response.bindings(0).role(), "roles/spanner.databaseReader");

  iam_api::GetIamPolicyRequest get_request;
  get_request.set_resource(resource);
  iam_api::Policy get_response;
  grpc::ClientContext get_context;
  grpc::Status get_status = test_env()->instance_admin_client()->GetIamPolicy(
      &get_context, get_request, &get_response);
  ASSERT_TRUE(get_status.ok()) << get_status.error_message();
  EXPECT_EQ(get_response.SerializeAsString(), set_response.SerializeAsString());

  iam_api::TestIamPermissionsRequest permissions_request;
  permissions_request.set_resource(resource);
  permissions_request.add_permissions("spanner.instances.get");
  permissions_request.add_permissions("spanner.databases.list");
  iam_api::TestIamPermissionsResponse permissions_response;
  grpc::ClientContext permissions_context;
  grpc::Status permissions_status =
      test_env()->instance_admin_client()->TestIamPermissions(
          &permissions_context, permissions_request, &permissions_response);
  ASSERT_TRUE(permissions_status.ok()) << permissions_status.error_message();
  ASSERT_EQ(permissions_response.permissions_size(), 2);
  EXPECT_EQ(permissions_response.permissions(0), "spanner.instances.get");
  EXPECT_EQ(permissions_response.permissions(1), "spanner.databases.list");
}

TEST_F(PolicyApiTest, DatabasePoliciesRoundTripWithCallerEtag) {
  const std::string resource =
      "projects/test-project/instances/test-instance/databases/test-database";
  iam_api::SetIamPolicyRequest set_request;
  set_request.set_resource(resource);
  set_request.mutable_policy()->set_etag("caller-etag");
  auto* binding = set_request.mutable_policy()->add_bindings();
  binding->set_role("roles/spanner.databaseUser");
  binding->add_members("serviceAccount:local@example.com");

  iam_api::Policy set_response;
  grpc::ClientContext set_context;
  grpc::Status set_status = test_env()->database_admin_client()->SetIamPolicy(
      &set_context, set_request, &set_response);
  ASSERT_TRUE(set_status.ok()) << set_status.error_message();
  EXPECT_EQ(set_response.etag(), "caller-etag");

  iam_api::GetIamPolicyRequest get_request;
  get_request.set_resource(resource);
  iam_api::Policy get_response;
  grpc::ClientContext get_context;
  grpc::Status get_status = test_env()->database_admin_client()->GetIamPolicy(
      &get_context, get_request, &get_response);
  ASSERT_TRUE(get_status.ok()) << get_status.error_message();
  EXPECT_EQ(get_response.SerializeAsString(), set_response.SerializeAsString());
}

TEST_F(PolicyApiTest, MissingPolicyIsEmptyAndResourceIsRequired) {
  iam_api::GetIamPolicyRequest get_request;
  get_request.set_resource("projects/test-project/instances/test-instance");
  iam_api::Policy get_response;
  grpc::ClientContext get_context;
  grpc::Status get_status = test_env()->instance_admin_client()->GetIamPolicy(
      &get_context, get_request, &get_response);
  ASSERT_TRUE(get_status.ok()) << get_status.error_message();
  EXPECT_EQ(get_response.ByteSizeLong(), 0);

  iam_api::TestIamPermissionsRequest invalid_request;
  invalid_request.add_permissions("spanner.instances.get");
  iam_api::TestIamPermissionsResponse invalid_response;
  grpc::ClientContext invalid_context;
  grpc::Status invalid_status =
      test_env()->instance_admin_client()->TestIamPermissions(
          &invalid_context, invalid_request, &invalid_response);
  EXPECT_EQ(invalid_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(PolicyPersistenceTest, PoliciesHydrateIntoNewEnvironment) {
  PolicyPersistentDataDirectory data_dir;
  const std::string instance = "projects/p/instances/i1";
  const std::string database = instance + "/databases/d1";

  iam_api::Policy instance_policy;
  instance_policy.set_version(3);
  instance_policy.set_etag("instance-etag");
  auto* instance_binding = instance_policy.add_bindings();
  instance_binding->set_role("roles/spanner.viewer");
  instance_binding->add_members("user:instance@example.com");

  iam_api::Policy database_policy;
  database_policy.set_version(1);
  database_policy.set_etag("database-etag");
  auto* database_binding = database_policy.add_bindings();
  database_binding->set_role("roles/spanner.databaseUser");
  database_binding->add_members("user:database@example.com");

  {
    test::TestEnv first;
    CreatePolicyResource(&first, "projects/p", "i1", "d1");
    iam_api::SetIamPolicyRequest request;
    iam_api::Policy response;
    grpc::ClientContext instance_context;
    request.set_resource(instance);
    *request.mutable_policy() = instance_policy;
    ASSERT_TRUE(first.instance_admin_client()
                    ->SetIamPolicy(&instance_context, request, &response)
                    .ok());

    grpc::ClientContext database_context;
    request.set_resource(database);
    *request.mutable_policy() = database_policy;
    ASSERT_TRUE(first.database_admin_client()
                    ->SetIamPolicy(&database_context, request, &response)
                    .ok());
  }

  {
    test::TestEnv restored;
    MetadataStore* metadata = restored.server()->env()->metadata_store();
    ASSERT_NE(metadata, nullptr);
    GOOGLESQL_ASSERT_OK(metadata->Load());
    CreatePolicyResource(&restored, "projects/p", "i1", "d1");
    for (const auto& [resource, policy] : metadata->AllIamPolicies()) {
      restored.server()->env()->SetIamPolicy(resource, policy);
    }

    iam_api::GetIamPolicyRequest get_request;
    iam_api::Policy get_response;
    grpc::ClientContext instance_context;
    get_request.set_resource(instance);
    ASSERT_TRUE(restored.instance_admin_client()
                    ->GetIamPolicy(&instance_context, get_request, &get_response)
                    .ok());
    EXPECT_EQ(get_response.SerializeAsString(),
              instance_policy.SerializeAsString());

    grpc::ClientContext database_context;
    get_request.set_resource(database);
    ASSERT_TRUE(restored.database_admin_client()
                    ->GetIamPolicy(&database_context, get_request, &get_response)
                    .ok());
    EXPECT_EQ(get_response.SerializeAsString(),
              database_policy.SerializeAsString());

    iam_api::TestIamPermissionsRequest permissions_request;
    permissions_request.set_resource(database);
    permissions_request.add_permissions("spanner.databases.get");
    permissions_request.add_permissions("spanner.sessions.create");
    iam_api::TestIamPermissionsResponse permissions_response;
    grpc::ClientContext permissions_context;
    ASSERT_TRUE(restored.database_admin_client()
                    ->TestIamPermissions(&permissions_context,
                                         permissions_request,
                                         &permissions_response)
                    .ok());
    ASSERT_EQ(permissions_response.permissions_size(), 2);
    EXPECT_EQ(permissions_response.permissions(0), "spanner.databases.get");
    EXPECT_EQ(permissions_response.permissions(1), "spanner.sessions.create");
  }
}

TEST(PolicyPersistenceTest, ResourceDeletionRemovesPolicies) {
  PolicyPersistentDataDirectory data_dir;
  test::TestEnv env;
  const std::string instance = "projects/p/instances/i1";
  const std::string database = instance + "/databases/d1";

  instance_api::CreateInstanceRequest create_instance;
  create_instance.set_parent("projects/p");
  create_instance.set_instance_id("i1");
  create_instance.mutable_instance()->set_config("emulator-config");
  operations_api::Operation operation;
  grpc::ClientContext create_instance_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->CreateInstance(&create_instance_context, create_instance,
                                   &operation)
                  .ok());

  database_api::CreateDatabaseRequest create_database;
  create_database.set_parent(instance);
  create_database.set_create_statement("CREATE DATABASE `d1`");
  grpc::ClientContext create_database_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->CreateDatabase(&create_database_context, create_database,
                                   &operation)
                  .ok());
  const std::filesystem::path database_root =
      std::filesystem::path(data_dir.path()) / "projects" / "p" / "instances" /
      "i1" / "databases" / "d1";
  EXPECT_TRUE(std::filesystem::exists(database_root / "storage"));

  iam_api::SetIamPolicyRequest set_request;
  set_request.mutable_policy()->set_etag("policy");
  iam_api::Policy policy;
  grpc::ClientContext set_instance_context;
  set_request.set_resource(instance);
  ASSERT_TRUE(env.instance_admin_client()
                  ->SetIamPolicy(&set_instance_context, set_request, &policy)
                  .ok());
  grpc::ClientContext set_database_context;
  set_request.set_resource(database);
  ASSERT_TRUE(env.database_admin_client()
                  ->SetIamPolicy(&set_database_context, set_request, &policy)
                  .ok());
  const std::string child_resource = database + "/backupSchedules/s1";
  env.server()->env()->SetIamPolicy(child_resource, set_request.policy());
  env.server()->env()->metadata_store()->SetIamPolicy(child_resource,
                                                      set_request.policy());
  GOOGLESQL_ASSERT_OK(env.server()->env()->metadata_store()->Save());

  database_api::DropDatabaseRequest drop_database;
  drop_database.set_database(database);
  protobuf_api::Empty empty;
  grpc::ClientContext drop_database_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->DropDatabase(&drop_database_context, drop_database, &empty)
                  .ok());
  EXPECT_FALSE(std::filesystem::exists(database_root));

  iam_api::GetIamPolicyRequest get_request;
  get_request.set_resource(database);
  grpc::ClientContext get_database_context;
  grpc::Status get_database_status = env.database_admin_client()->GetIamPolicy(
      &get_database_context, get_request, &policy);
  EXPECT_EQ(get_database_status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_FALSE(env.server()
                   ->env()
                   ->metadata_store()
                   ->GetIamPolicy(database)
                   .has_value());
  get_request.set_resource(child_resource);
  grpc::ClientContext get_child_context;
  grpc::Status get_child_status = env.database_admin_client()->GetIamPolicy(
      &get_child_context, get_request, &policy);
  EXPECT_EQ(get_child_status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_FALSE(env.server()
                   ->env()
                   ->metadata_store()
                   ->GetIamPolicy(child_resource)
                   .has_value());

  instance_api::DeleteInstanceRequest delete_instance;
  delete_instance.set_name(instance);
  grpc::ClientContext delete_instance_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->DeleteInstance(&delete_instance_context, delete_instance,
                                   &empty)
                  .ok());
  get_request.set_resource(instance);
  grpc::ClientContext get_instance_context;
  grpc::Status get_instance_status = env.instance_admin_client()->GetIamPolicy(
      &get_instance_context, get_request, &policy);
  EXPECT_EQ(get_instance_status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_FALSE(env.server()
                   ->env()
                   ->metadata_store()
                   ->GetIamPolicy(instance)
                   .has_value());
}

TEST(PolicyPersistenceTest, SaveFailureRollsBackMemoryAndMetadata) {
  PolicyPersistentDataDirectory data_dir;
  test::TestEnv env;
  CreatePolicyResource(&env, "projects/p", "i1");
  CreatePolicyResource(&env, "projects/p", "i2", "d1");

  const std::string resource = "projects/p/instances/i1";

  iam_api::SetIamPolicyRequest request;
  request.set_resource(resource);
  request.mutable_policy()->set_version(1);
  request.mutable_policy()->set_etag("original");
  request.mutable_policy()->add_bindings()->set_role("roles/spanner.viewer");
  iam_api::Policy response;
  grpc::ClientContext first_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->SetIamPolicy(&first_context, request, &response)
                  .ok());
  const iam_api::Policy original = response;

  const std::string child_resource =
      "projects/p/instances/i2/databases/d1";
  request.set_resource(child_resource);
  request.mutable_policy()->Clear();
  request.mutable_policy()->set_version(1);
  request.mutable_policy()->set_etag("child");
  grpc::ClientContext child_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->SetIamPolicy(&child_context, request, &response)
                  .ok());
  const iam_api::Policy child_policy = response;

  ASSERT_TRUE(
      std::filesystem::create_directory(data_dir.path() + "/metadata.json.tmp"));
  request.set_resource(resource);
  request.mutable_policy()->set_version(3);
  request.mutable_policy()->set_etag("replacement");
  grpc::ClientContext failed_context;
  grpc::Status status = env.instance_admin_client()->SetIamPolicy(
      &failed_context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  request.set_resource("projects/p/instances/i2");
  grpc::ClientContext absent_parent_context;
  status = env.instance_admin_client()->SetIamPolicy(
      &absent_parent_context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  iam_api::GetIamPolicyRequest get_request;
  get_request.set_resource(resource);
  grpc::ClientContext get_context;
  ASSERT_TRUE(env.instance_admin_client()
                  ->GetIamPolicy(&get_context, get_request, &response)
                  .ok());
  EXPECT_EQ(response.SerializeAsString(), original.SerializeAsString());

  auto persisted =
      env.server()->env()->metadata_store()->GetIamPolicy(resource);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(persisted->SerializeAsString(), original.SerializeAsString());

  get_request.set_resource(child_resource);
  grpc::ClientContext child_get_context;
  ASSERT_TRUE(env.database_admin_client()
                  ->GetIamPolicy(&child_get_context, get_request, &response)
                  .ok());
  EXPECT_EQ(response.SerializeAsString(), child_policy.SerializeAsString());
  EXPECT_FALSE(env.server()
                   ->env()
                   ->metadata_store()
                   ->GetIamPolicy("projects/p/instances/i2")
                   .has_value());
}

TEST(PolicyPersistenceTest, ConcurrentUpdatesKeepLiveAndDiskStateEqual) {
  PolicyPersistentDataDirectory data_dir;
  test::TestEnv env;
  constexpr int kRounds = 16;
  constexpr int kWriters = 8;

  for (int round = 0; round < kRounds; ++round) {
    const std::string resource =
        "projects/p/instances/concurrent-" + std::to_string(round);
    CreatePolicyResource(&env, "projects/p",
                         "concurrent-" + std::to_string(round));
    std::atomic<bool> start = false;
    std::vector<grpc::Status> statuses(kWriters);
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int writer = 0; writer < kWriters; ++writer) {
      writers.emplace_back([&, writer] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        iam_api::SetIamPolicyRequest request;
        request.set_resource(resource);
        request.mutable_policy()->set_etag(
            "writer-" + std::to_string(writer));
        request.mutable_policy()->add_bindings()->set_role(
            "roles/spanner.viewer");
        iam_api::Policy response;
        grpc::ClientContext context;
        statuses[writer] = env.instance_admin_client()->SetIamPolicy(
            &context, request, &response);
      });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
      writer.join();
    }
    for (const grpc::Status& status : statuses) {
      ASSERT_TRUE(status.ok()) << status.error_message();
    }

    const auto live = env.server()->env()->GetIamPolicy(resource);
    ASSERT_TRUE(live.has_value());
    MetadataStore reloaded(data_dir.path());
    GOOGLESQL_ASSERT_OK(reloaded.Load());
    const auto persisted = reloaded.GetIamPolicy(resource);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(persisted->SerializeAsString(), live->SerializeAsString());
  }
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
