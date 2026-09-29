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

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/spanner/v1/keys.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/spanner/v1/transaction.pb.h"
#include "google/spanner/v1/type.pb.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "backend/access/read.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "common/errors.h"
#include "frontend/converters/keys.h"
#include "frontend/converters/partition.h"
#include "frontend/converters/query.h"
#include "frontend/converters/values.h"
#include "frontend/entities/session.h"
#include "frontend/entities/transaction.h"
#include "frontend/proto/partition_token.pb.h"
#include "frontend/server/handler.h"
#include "frontend/server/request_context.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace spanner_api = ::google::spanner::v1;

namespace {

struct SessionAndTransaction {
  std::shared_ptr<Session> session;
  std::shared_ptr<Transaction> transaction;
};

// The number of empty partitions to generate for StreamingPartitionQuery.
// This is to simulate production behavior where some partitions may be empty.
// The number 5 is chosen arbitrarily to generate a few empty partitions.
const int kNumEmptyPartitions = 5;

absl::Status ValidateTransactionSelectorForPartitionRead(
    const spanner_api::TransactionSelector& selector) {
  // PartitionRead and PartitionQuery only support read only snapshot
  // transactions. Read/write and single use transactions are not supported.
  switch (selector.selector_case()) {
    case spanner_api::TransactionSelector::SelectorCase::kBegin: {
      if (!selector.begin().has_read_only()) {
        return error::PartitionReadNeedsReadOnlyTxn();
      }
      return absl::OkStatus();
    }
    case spanner_api::TransactionSelector::SelectorCase::kId: {
      return absl::OkStatus();
    }
    case spanner_api::TransactionSelector::SelectorCase::kSingleUse:
      return error::PartitionReadDoesNotSupportSingleUseTransaction();
    case spanner_api::TransactionSelector::SELECTOR_NOT_SET:
      return error::MissingRequiredFieldError("TransactionSelector.selector");
  }
}

absl::Status ValidatePartitionOptions(
    const spanner_api::PartitionOptions& partition_options) {
  if (partition_options.partition_size_bytes() < 0) {
    return error::InvalidBytesPerBatch("partition_options");
  }
  if (partition_options.max_partitions() < 0) {
    return error::InvalidMaxPartitionCount("partition_options");
  }
  return absl::OkStatus();
}

// Set Session and Transaction for PartitionQuery/StreamingPartitionQuery.
absl::StatusOr<SessionAndTransaction> SetBasePartitionOptions(
    RequestContext* ctx,
    const ::google::spanner::v1::TransactionSelector& transaction,
    const std::string& session_name) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, session_name));

  // Get underlying transaction.
  GOOGLESQL_RETURN_IF_ERROR(ValidateTransactionSelectorForPartitionRead(transaction));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(transaction));
  if (!txn->IsReadOnly()) {
    return error::PartitionReadNeedsReadOnlyTxn();
  }

  return SessionAndTransaction{.session = std::move(session),
                               .transaction = std::move(txn)};
}

// Create a partition token for the given partition read request and partitioned
// key set.
absl::StatusOr<PartitionToken> CreatePartitionTokenForRead(
    const google::spanner::v1::PartitionReadRequest& request,
    const backend::TransactionID& txn_id,
    const google::spanner::v1::KeySet& partitioned_key_set) {
  PartitionToken partition_token;
  *partition_token.mutable_session() = request.session();
  *partition_token.mutable_transaction_id() = std::to_string(txn_id);

  auto read_params = partition_token.mutable_read_params();
  *read_params->mutable_table() = request.table();
  *read_params->mutable_index() = request.index();
  *read_params->mutable_key_set() = request.key_set();
  *read_params->mutable_columns() = request.columns();

  *partition_token.mutable_partitioned_key_set() = partitioned_key_set;
  return partition_token;
}

// Create a partition token for the given partition query request.
absl::StatusOr<PartitionToken> CreatePartitionTokenForQuery(
    const google::spanner::v1::PartitionQueryRequest& request,
    const backend::TransactionID& txn_id, bool empty_partition,
    int32_t partition_index = 0, int32_t num_partitions = 1) {
  if (request.sql().empty()) {
    return error::MissingRequiredFieldError("sql");
  }
  PartitionToken partition_token;
  *partition_token.mutable_session() = request.session();
  *partition_token.mutable_transaction_id() = std::to_string(txn_id);

  auto query_params = partition_token.mutable_query_params();
  *query_params->mutable_sql() = request.sql();
  *query_params->mutable_params() = request.params();
  *query_params->mutable_param_types() = request.param_types();

  partition_token.set_empty_query_partition(empty_partition);
  if (num_partitions > 1) {
    partition_token.set_query_partition_index(partition_index);
    partition_token.set_query_num_partitions(num_partitions);
  }
  return partition_token;
}

absl::StatusOr<spanner_api::Partition> CreateStreamingPartitionTokenForQuery(
    bool empty_partition) {
  StreamingPartitionToken partition_token;
  if (empty_partition) {
    partition_token.set_empty_query_partition(true);
  }

  spanner_api::Partition partition;
  GOOGLESQL_ASSIGN_OR_RETURN(*partition.mutable_partition_token(),
                   StreamingPartitionTokenToString(partition_token));
  return partition;
}

}  //  namespace

// Creates a set of partition tokens for executing parallel read operations.
absl::Status PartitionRead(RequestContext* ctx,
                           const spanner_api::PartitionReadRequest* request,
                           spanner_api::PartitionResponse* response) {
  // Take shared ownerships of session and transaction so that they will keep
  // valid throughout this function.
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Session> session,
                   GetSession(ctx, request->session()));

  // Get underlying transaction.
  GOOGLESQL_RETURN_IF_ERROR(
      ValidateTransactionSelectorForPartitionRead(request->transaction()));
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<Transaction> txn,
                   session->FindOrInitTransaction(request->transaction()));
  if (!txn->IsReadOnly()) {
    return error::PartitionReadNeedsReadOnlyTxn();
  }

  if (request->has_partition_options()) {
    GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionOptions(request->partition_options()));
  }

  if (ShouldReturnTransaction(request->transaction())) {
    GOOGLESQL_ASSIGN_OR_RETURN(*response->mutable_transaction(), txn->ToProto());
  }

  int64_t max_partitions = 0;
  if (request->has_partition_options()) {
    max_partitions = request->partition_options().max_partitions();
  }

  if (max_partitions > 1) {
    // If request contains discrete keys, distribute them across partitions.
    if (request->key_set().keys_size() > 1) {
      int64_t num_keys = request->key_set().keys_size();
      int64_t num_parts = std::min(num_keys, max_partitions);
      std::vector<spanner_api::KeySet> partition_key_sets(num_parts);
      for (int i = 0; i < num_keys; ++i) {
        *partition_key_sets[i % num_parts].add_keys() = request->key_set().keys(i);
      }
      for (int i = 0; i < num_parts; ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            auto token,
            CreatePartitionTokenForRead(*request, txn->id(), partition_key_sets[i]));
        spanner_api::Partition part;
        GOOGLESQL_ASSIGN_OR_RETURN(*part.mutable_partition_token(),
                         PartitionTokenToString(token));
        *response->mutable_partitions()->Add() = std::move(part);
      }
      return absl::OkStatus();
    }

    // If request contains multiple ranges, distribute them across partitions.
    if (request->key_set().ranges_size() > 1) {
      int64_t num_ranges = request->key_set().ranges_size();
      int64_t num_parts = std::min(num_ranges, max_partitions);
      std::vector<spanner_api::KeySet> partition_key_sets(num_parts);
      for (int i = 0; i < num_ranges; ++i) {
        *partition_key_sets[i % num_parts].add_ranges() = request->key_set().ranges(i);
      }
      for (int i = 0; i < num_parts; ++i) {
        GOOGLESQL_ASSIGN_OR_RETURN(
            auto token,
            CreatePartitionTokenForRead(*request, txn->id(), partition_key_sets[i]));
        spanner_api::Partition part;
        GOOGLESQL_ASSIGN_OR_RETURN(*part.mutable_partition_token(),
                         PartitionTokenToString(token));
        *response->mutable_partitions()->Add() = std::move(part);
      }
      return absl::OkStatus();
    }

    // For key_set.all() (or single range covering table), read primary keys to slice table.
    const backend::Table* table = txn->schema()->FindTable(request->table());
    if (table != nullptr) {
      if (!request->index().empty()) {
        const backend::Index* index = txn->schema()->FindIndex(request->index());
        if (index != nullptr) {
          table = index->index_data_table();
        }
      }
    }
    if (table != nullptr) {
      backend::ReadArg pk_read_arg;
      pk_read_arg.table = request->table();
      pk_read_arg.index = request->index();
      for (const auto* col : table->primary_key()) {
        pk_read_arg.columns.push_back(col->column()->Name());
      }
      pk_read_arg.key_set = backend::KeySet::All();
      std::unique_ptr<backend::RowCursor> cursor;
      auto read_status = txn->read_only()->Read(pk_read_arg, &cursor);
      if (read_status.ok() && cursor != nullptr) {
        std::vector<google::protobuf::ListValue> row_keys;
        while (cursor->Next()) {
          google::protobuf::ListValue list_val;
          bool conv_ok = true;
          for (int c = 0; c < cursor->NumColumns(); ++c) {
            auto val_status = ValueToProto(cursor->ColumnValue(c));
            if (!val_status.ok()) {
              conv_ok = false;
              break;
            }
            *list_val.add_values() = std::move(val_status.value());
          }
          if (conv_ok) {
            row_keys.push_back(std::move(list_val));
          }
        }
        if (row_keys.size() >= 2) {
          int64_t num_rows = row_keys.size();
          int64_t num_parts = std::min(num_rows, max_partitions);
          std::vector<spanner_api::KeySet> partition_key_sets(num_parts);
          for (size_t i = 0; i < num_rows; ++i) {
            *partition_key_sets[i % num_parts].add_keys() = std::move(row_keys[i]);
          }
          for (int i = 0; i < num_parts; ++i) {
            GOOGLESQL_ASSIGN_OR_RETURN(
                auto token,
                CreatePartitionTokenForRead(*request, txn->id(), partition_key_sets[i]));
            spanner_api::Partition part;
            GOOGLESQL_ASSIGN_OR_RETURN(*part.mutable_partition_token(),
                             PartitionTokenToString(token));
            *response->mutable_partitions()->Add() = std::move(part);
          }
          return absl::OkStatus();
        }
      }
    }
  }

  // Add two partitions to result set, with first partition being empty.
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto empty_partition_token,
      CreatePartitionTokenForRead(*request, txn->id(), spanner_api::KeySet()));
  spanner_api::Partition empty_partition;
  GOOGLESQL_ASSIGN_OR_RETURN(*empty_partition.mutable_partition_token(),
                   PartitionTokenToString(empty_partition_token));

  // Second partition contains full result set for requested key_set.
  GOOGLESQL_ASSIGN_OR_RETURN(
      auto full_partition_token,
      CreatePartitionTokenForRead(*request, txn->id(), request->key_set()));
  spanner_api::Partition full_partition;
  GOOGLESQL_ASSIGN_OR_RETURN(*full_partition.mutable_partition_token(),
                   PartitionTokenToString(full_partition_token));

  *response->mutable_partitions()->Add() = empty_partition;
  *response->mutable_partitions()->Add() = full_partition;

  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Spanner, PartitionRead);

// Creates a set of partition tokens for executing parallel query operations.
absl::Status PartitionQuery(RequestContext* ctx,
                            const spanner_api::PartitionQueryRequest* request,
                            spanner_api::PartitionResponse* response) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      SessionAndTransaction session_and_txn,
      SetBasePartitionOptions(ctx, request->transaction(), request->session()));
  auto session = session_and_txn.session;
  auto txn = session_and_txn.transaction;

  if (request->has_partition_options()) {
    GOOGLESQL_RETURN_IF_ERROR(ValidatePartitionOptions(request->partition_options()));
  }

  if (ShouldReturnTransaction(request->transaction())) {
    GOOGLESQL_ASSIGN_OR_RETURN(*response->mutable_transaction(), txn->ToProto());
  }

  // check query is partitionable.
  GOOGLESQL_ASSIGN_OR_RETURN(
      backend::Query query,
      QueryFromProto(request->sql(), request->params(), request->param_types(),
                     txn->query_engine()->type_factory()
                     ,
                     txn->schema()->proto_bundle()
                     ));
  GOOGLESQL_RETURN_IF_ERROR(txn->query_engine()->IsPartitionable(
      query,
      backend::QueryContext{
          .schema = txn->schema(), .reader = nullptr, .writer = nullptr}));

  int64_t max_partitions = 0;
  if (request->has_partition_options()) {
    max_partitions = request->partition_options().max_partitions();
  }

  if (max_partitions > 1) {
    for (int32_t i = 0; i < max_partitions; ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          auto partition_token,
          CreatePartitionTokenForQuery(*request, txn->id(),
                                       /*empty_partition=*/false,
                                       /*partition_index=*/i,
                                       /*num_partitions=*/max_partitions));
      spanner_api::Partition partition;
      GOOGLESQL_ASSIGN_OR_RETURN(*partition.mutable_partition_token(),
                       PartitionTokenToString(partition_token));
      *response->mutable_partitions()->Add() = std::move(partition);
    }
  } else {
    // Add two partitions to result set, with first partition being empty.
    GOOGLESQL_ASSIGN_OR_RETURN(auto empty_partition_token,
                     CreatePartitionTokenForQuery(*request, txn->id(),
                                                  /*empty_partition=*/true));
    spanner_api::Partition empty_partition;
    GOOGLESQL_ASSIGN_OR_RETURN(*empty_partition.mutable_partition_token(),
                     PartitionTokenToString(empty_partition_token));

    // Second partition contains full result set for requested query.
    GOOGLESQL_ASSIGN_OR_RETURN(auto full_partition_token,
                     CreatePartitionTokenForQuery(*request, txn->id(),
                                                  /*empty_partition=*/false));
    spanner_api::Partition full_partition;
    GOOGLESQL_ASSIGN_OR_RETURN(*full_partition.mutable_partition_token(),
                     PartitionTokenToString(full_partition_token));

    *response->mutable_partitions()->Add() = empty_partition;
    *response->mutable_partitions()->Add() = full_partition;
  }

  return absl::OkStatus();
}
REGISTER_GRPC_HANDLER(Spanner, PartitionQuery);

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
