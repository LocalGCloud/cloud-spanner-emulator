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

#include "backend/schema/verifiers/placement_verifiers.h"

#include <memory>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "backend/datamodel/key_range.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/storage/iterator.h"
#include "backend/storage/storage.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

const Column* GetPlacementKeyColumn(const Table* table) {
  for (const Column* column : table->columns()) {
    if (column->is_placement_key()) {
      return column;
    }
  }
  return nullptr;
}

absl::Status VerifyPlacementNotInUse(const Storage* storage,
                                     absl::Time timestamp,
                                     const Schema* schema,
                                     absl::string_view placement_name) {
  for (const Table* table : schema->tables()) {
    const Column* placement_key = GetPlacementKeyColumn(table);
    if (placement_key == nullptr) {
      continue;
    }
    std::unique_ptr<StorageIterator> itr;
    GOOGLESQL_RETURN_IF_ERROR(storage->Read(timestamp, table->id(), KeyRange::All(),
                                  {placement_key->id()}, &itr));
    while (itr->Next()) {
      const googlesql::Value& value = itr->ColumnValue(0);
      if (value.is_valid() && !value.is_null() && value.type()->IsString() &&
          value.string_value() == placement_name) {
        return error::PlacementInUse(placement_name, table->Name());
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(itr->Status());
  }
  return absl::OkStatus();
}

absl::Status VerifyPlacementTableIsEmpty(const Storage* storage,
                                         absl::Time timestamp,
                                         const Table* table) {
  const Column* placement_key = GetPlacementKeyColumn(table);
  if (placement_key == nullptr) {
    return absl::OkStatus();
  }
  std::unique_ptr<StorageIterator> itr;
  GOOGLESQL_RETURN_IF_ERROR(storage->Read(timestamp, table->id(), KeyRange::All(),
                                {placement_key->id()}, &itr));
  if (itr->Next()) {
    return error::DropNonEmptyPlacementTable(table->Name());
  }
  return itr->Status();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
