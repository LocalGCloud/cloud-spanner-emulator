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

#include <limits>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "common/errors.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/conformance/common/database_test_base.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using testing::HasSubstr;
using googlesql_base::testing::StatusIs;

class ANNTest : public DatabaseTest {
 public:
  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchemaFromFile("ann.test"));
    return PopulateDatabase();
  }

 protected:
  absl::Status PopulateDatabase() {
    GOOGLESQL_RETURN_IF_ERROR(
        MultiInsert(
            "Base",
            {"MyKey", "MyData", "Embedding", "Embedding2", "Embedding3"},
            {{1, "datastr", std::vector<float>{1.0, 0.8},
              std::vector<float>{1.0, 0.8}, std::vector<double>{1.0, 0.8}},
             {2, "datastr", std::vector<float>{0.1, 1.0},
              std::vector<float>{0.1, 1.0}, std::vector<double>{0.1, 1.0}}})
            .status());
    return absl::OkStatus();
  }
};

TEST_F(ANNTest, ExactDistanceFunctions) {
  EXPECT_THAT(Query(R"sql(SELECT COSINE_DISTANCE(
      ARRAY<FLOAT32>[1.0, 0.0], ARRAY<FLOAT32>[0.0, 1.0]))sql"),
              IsOkAndHoldsRows({{1.0}}));
  EXPECT_THAT(Query(R"sql(SELECT COSINE_DISTANCE(
      ARRAY<FLOAT64>[1.0, 0.0], ARRAY<FLOAT64>[0.0, 1.0]))sql"),
              IsOkAndHoldsRows({{1.0}}));
  EXPECT_THAT(Query(R"sql(SELECT EUCLIDEAN_DISTANCE(
      ARRAY<FLOAT32>[3.0, 4.0], ARRAY<FLOAT32>[0.0, 0.0]))sql"),
              IsOkAndHoldsRows({{5.0}}));
  EXPECT_THAT(Query(R"sql(SELECT EUCLIDEAN_DISTANCE(
      ARRAY<FLOAT64>[3.0, 4.0], ARRAY<FLOAT64>[0.0, 0.0]))sql"),
              IsOkAndHoldsRows({{5.0}}));
  EXPECT_THAT(Query(R"sql(SELECT DOT_PRODUCT(
      ARRAY<FLOAT32>[1.0, 2.0], ARRAY<FLOAT32>[3.0, 4.0]))sql"),
              IsOkAndHoldsRows({{11.0}}));
  EXPECT_THAT(Query(R"sql(SELECT DOT_PRODUCT(
      ARRAY<FLOAT64>[1.0, 2.0], ARRAY<FLOAT64>[3.0, 4.0]))sql"),
              IsOkAndHoldsRows({{11.0}}));
  EXPECT_THAT(Query(R"sql(SELECT MyKey FROM Base
      ORDER BY COSINE_DISTANCE(Embedding, ARRAY<FLOAT32>[1.0, 0.1])
      LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(Query(R"sql(SELECT MyKey FROM Base
      ORDER BY EUCLIDEAN_DISTANCE(Embedding3, ARRAY<FLOAT64>[1.0, 0.1])
      LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));

  EXPECT_THAT(Query(R"sql(SELECT COSINE_DISTANCE(
      CAST(NULL AS ARRAY<FLOAT32>), ARRAY<FLOAT32>[1.0]))sql"),
              IsOkAndHoldsRows({{Null<double>()}}));
  EXPECT_FALSE(Query(R"sql(SELECT COSINE_DISTANCE(
      ARRAY<FLOAT32>[0.0, 0.0], ARRAY<FLOAT32>[1.0, 0.0]))sql").ok());
  EXPECT_FALSE(Query(R"sql(SELECT COSINE_DISTANCE(
      ARRAY<FLOAT64>[], ARRAY<FLOAT64>[]))sql").ok());
  EXPECT_FALSE(Query(R"sql(SELECT EUCLIDEAN_DISTANCE(
      ARRAY<FLOAT64>[1.0], ARRAY<FLOAT64>[1.0, 2.0]))sql").ok());
  EXPECT_FALSE(Query(R"sql(SELECT DOT_PRODUCT(
      ARRAY<FLOAT64>[1.0, NULL], ARRAY<FLOAT64>[1.0, 2.0]))sql").ok());
  EXPECT_THAT(Query(R"sql(SELECT DOT_PRODUCT(
      ARRAY<FLOAT64>[CAST('NaN' AS FLOAT64)], ARRAY<FLOAT64>[1.0]))sql"),
              IsOkAndHoldsRows({{std::numeric_limits<double>::quiet_NaN()}}));
  EXPECT_THAT(Query(R"sql(SELECT EUCLIDEAN_DISTANCE(
      ARRAY<FLOAT64>[CAST('inf' AS FLOAT64)], ARRAY<FLOAT64>[1.0]))sql"),
              IsOkAndHoldsRows({{std::numeric_limits<double>::infinity()}}));
  EXPECT_FALSE(Query(R"sql(SELECT COSINE_DISTANCE(
      ARRAY<FLOAT64>[1e300, 1e300], ARRAY<FLOAT64>[1e300, -1e300]))sql").ok());
}

TEST_F(ANNTest, BasicANNQuery) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            ARRAY<FLOAT32>[1.0, 0.1], b.Embedding,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(ANNTest, BasicANNQueryWithParams) {
  EXPECT_THAT(QueryWithParams(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index_double} b
          WHERE b.Embedding3 IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            @embedding, b.Embedding3,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql",
                  {{"embedding", Value(std::vector<double>{1.0, 0.1})}}),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(QueryWithParams(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index_double} b
          WHERE b.Embedding3 IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding3, @embedding,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql",
                  {{"embedding", Value(std::vector<double>{1.0, 0.1})}}),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(ANNTest, ANNQueryNoForceIndex) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_DOT_PRODUCT(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{2}, {1}}));
}

TEST_F(ANNTest, ANNQueryNoForceIndexDifferentColumn) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding2 IS NOT NULL
          ORDER BY APPROX_EUCLIDEAN_DISTANCE(
            b.Embedding2, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(ANNTest, ANNQueryWrongDistanceType) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_EUCLIDEAN_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableForceIndexWrongDistanceType(
          "vec_index", "COSINE", "APPROX_EUCLIDEAN_DISTANCE", "Embedding"));
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_EUCLIDEAN_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::HasSubstr(
                   "No usable vector index can be found for this query")));
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding2 IS NOT NULL
          ORDER BY APPROX_DOT_PRODUCT(
            b.Embedding2, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::HasSubstr(
                   "No usable vector index can be found for this query")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=index2} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_EUCLIDEAN_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              error::NotVectorIndexes("index2"));
}

TEST_F(ANNTest, ANNQueryWrongColumn) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding2 IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding2, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              error::VectorIndexesUnusableForceIndexWrongColumn(
                  "vec_index", "APPROX_COSINE_DISTANCE", "Embedding2"));
}

TEST_F(ANNTest, ANNQueryNoOptions) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1])
          LIMIT 2)sql"),
      error::ApproxDistanceFunctionOptionsRequired("approx_cosine_distance"));
}

TEST_F(ANNTest, ANNQueryWrongOptions) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves": 1}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::ContainsRegex(
                   "Argument `options` of function APPROX_COSINE_DISTANCE is "
                   "invalid.")));
}

TEST_F(ANNTest, ANNQueryInvalidNumLeavesToSearch) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": "abc"}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::ContainsRegex(
                   "Argument `options` of function APPROX_COSINE_DISTANCE is "
                   "invalid.")));
}

TEST_F(ANNTest, ANNQueryNegativeNumLeavesToSearch) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": -1}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               testing::ContainsRegex(
                   "Argument `options` of function APPROX_COSINE_DISTANCE is "
                   "invalid.")));
}

TEST_F(ANNTest, ANNQueryNoOrderBy) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
}

TEST_F(ANNTest, ANNQueryMustUnderOrderBy) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY b.MyKey
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
}

TEST_F(ANNTest, ANNQueryNoJoin) {
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
          CREATE TABLE Base2 (
            MyKey INT64 NOT NULL,
            MyData STRING(MAX),
            Embedding ARRAY<FLOAT32>(vector_length=>2),
          ) PRIMARY KEY(MyKey)
        )sql"}));
  GOOGLESQL_EXPECT_OK(MultiInsert("Base2", {"MyKey", "MyData", "Embedding"},
                        {{1, "datastr2", std::vector<float>{1.0, 0.8}},
                         {2, "datastr2", std::vector<float>{0.0, 1.0}}})
                .status());
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey, b2.MyData FROM Base@{FORCE_INDEX=vec_index} b
          JOIN Base2 b2 ON b.MyKey = b2.MyKey
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
}

TEST_F(ANNTest, ANNQueryOrderByNoOtherColumns) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}'),
            b.MyKey
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
}

TEST_F(ANNTest, ANNQueryComplexJoins) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT MyKey FROM Base@{FORCE_INDEX=index2}
          JOIN
          (
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2
          ) USING (MyKey)
          )sql"),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT MyKey FROM
          (
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2
          )
          JOIN
          (
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2
          ) USING (MyKey)
          ORDER BY MyKey ASC
          )sql"),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(ANNTest, ANNQueryMultipleWhere) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.MyKey != 1 AND b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{2}}));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE (b.Embedding2 IS NULL OR b.MyData IS NOT NULL) AND b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(ANNTest, BasicANNQueryNoWhere) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableNotNullFiltered("vec_index", "Embedding"));
}

TEST_F(ANNTest, BasicANNQueryWrongWhere) {
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding2 IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableNotNullFiltered("vec_index", "Embedding"));
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableNotNullFiltered("vec_index", "Embedding"));
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding2 IS NOT NULL AND b.Embedding IS NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableNotNullFiltered("vec_index", "Embedding"));
}

TEST_F(ANNTest, ANNQueryOffset) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base@{FORCE_INDEX=vec_index} b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2 OFFSET 1)sql"),
              IsOkAndHoldsRows({{2}}));
}

TEST_F(ANNTest, ANNQueryStoreColumn) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyData FROM Base@{FORCE_INDEX=vec_index_store} b
          WHERE b.Embedding IS NOT NULL AND b.MyData IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{"datastr"}, {"datastr"}}));
}

TEST_F(ANNTest, ANNQueryWrongInput) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<INT64>[1, 0],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("No matching signature for function "
                                 "APPROX_COSINE_DISTANCE")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            ARRAY<INT64>[1, 0], ARRAY<INT64>[1, 0],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("No matching signature for function "
                                 "APPROX_COSINE_DISTANCE")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT64>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("No matching signature for function "
                                 "APPROX_COSINE_DISTANCE")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            (SELECT 1), b.Embedding,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("No matching signature for function "
                                 "APPROX_COSINE_DISTANCE")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, b.Embedding,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, b.Embedding2,
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
  EXPECT_THAT(
      Query(
          R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[0.0, 0.0],
            options => JSON '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          HasSubstr("Cannot compute cosine distance against zero vector")));
}

TEST_F(ANNTest, ANNQueryNoLimit) {
  EXPECT_THAT(Query(
                  R"sql(
          SELECT b.MyKey FROM Base b
          WHERE b.Embedding IS NOT NULL
          ORDER BY APPROX_COSINE_DISTANCE(
            b.Embedding, ARRAY<FLOAT32>[1.0, 0.1],
            options => JSON '{"num_leaves_to_search": 1}')
          )sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("The use of function APPROX_COSINE_DISTANCE "
                                 "is not supported in this query")));
}

TEST_F(ANNTest, AlterVectorIndex) {
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
      CREATE VECTOR INDEX VI_alter ON Base(Embedding) WHERE Embedding IS NOT NULL
        OPTIONS(distance_type = 'EUCLIDEAN')
    )sql"}));
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
      ALTER VECTOR INDEX VI_alter ADD STORED COLUMN MyData
    )sql"}));
  EXPECT_THAT(SetSchema({
                  R"sql(
      ALTER VECTOR INDEX VI_alter ADD STORED COLUMN NonExistent
    )sql"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr(error::VectorIndexStoredColumnNotFound(
                                     "VI_alter", "NonExistent")
                                     .message())));
  EXPECT_THAT(SetSchema({
                  R"sql(
      ALTER VECTOR INDEX VI_alter ADD STORED COLUMN MyData
    )sql"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr(error::VectorIndexStoredColumnAlreadyExists(
                                     "VI_alter", "MyData")
                                     .message())));
  EXPECT_THAT(SetSchema({
                  R"sql(
      ALTER VECTOR INDEX VI_alter ADD STORED COLUMN MyKey
    )sql"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr(error::VectorIndexStoredColumnIsKey(
                                     "VI_alter", "MyKey", "Base")
                                     .message())));
  EXPECT_THAT(
      SetSchema({
          R"sql(
      ALTER VECTOR INDEX VI_alter DROP STORED COLUMN NonExistent
    )sql"}),
      StatusIs(
          absl::StatusCode::kNotFound,
          HasSubstr(
              error::ColumnNotFound("VI_alter", "NonExistent").message())));
  EXPECT_THAT(
      SetSchema({
          R"sql(
      ALTER VECTOR INDEX VI_alter DROP STORED COLUMN MyKey
    )sql"}),
      StatusIs(
          absl::StatusCode::kNotFound,
          HasSubstr(error::ColumnNotFound("VI_alter", "MyKey").message())));
  EXPECT_THAT(SetSchema({
                  R"sql(
      ALTER VECTOR INDEX VI_alter DROP STORED COLUMN Embedding
    )sql"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr(error::VectorIndexNotStoredColumn("VI_alter",
                                                                   "Embedding")
                                     .message())));
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
      ALTER VECTOR INDEX VI_alter DROP STORED COLUMN MyData
    )sql"}));
}

TEST_F(ANNTest, DropVectorIndex) {
  EXPECT_THAT(SetSchema({
                  R"sql(
      DROP VECTOR INDEX VI_drop
    )sql"}),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr(error::IndexNotFound("VI_drop").message())));
  EXPECT_THAT(SetSchema({
                  R"sql(
      DROP VECTOR INDEX index2
    )sql"}),
              StatusIs(absl::StatusCode::kNotFound,
                       HasSubstr(error::IndexNotFound("index2").message())));
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
      CREATE VECTOR INDEX VI_drop ON Base(Embedding) WHERE Embedding IS NOT NULL
        OPTIONS(distance_type = 'EUCLIDEAN')
    )sql"}));
  GOOGLESQL_EXPECT_OK(SetSchema({
      R"sql(
      DROP VECTOR INDEX VI_drop
    )sql"}));
}
// PostgreSQL creates vector indexes with CREATE INDEX ... USING scann and
// calls the approximate distance functions in the spanner namespace, passing
// the options as JSONB.
class PGANNTest : public DatabaseTest {
 public:
  PGANNTest() : feature_flags_({.enable_postgresql_interface = true}) {}

  void SetUp() override {
    dialect_ = database_api::DatabaseDialect::POSTGRESQL;
    DatabaseTest::SetUp();
  }

  absl::Status SetUpDatabase() override {
    GOOGLESQL_RETURN_IF_ERROR(SetSchemaFromFile("ann.test"));
    return MultiInsert(
               "base",
               {"mykey", "mydata", "embedding", "embedding2", "embedding3"},
               {{1, "datastr", std::vector<float>{1.0, 0.8},
                 std::vector<float>{1.0, 0.8}, std::vector<double>{1.0, 0.8}},
                {2, "datastr", std::vector<float>{0.1, 1.0},
                 std::vector<float>{0.1, 1.0}, std::vector<double>{0.1, 1.0}}})
        .status();
  }

 private:
  test::ScopedEmulatorFeatureFlagsSetter feature_flags_;
};

TEST_F(PGANNTest, BasicANNQuery) {
  EXPECT_THAT(Query(R"sql(
          SELECT b.mykey FROM base /*@ force_index=vec_index */ b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_cosine_distance(
            b.embedding, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(Query(R"sql(
          SELECT b.mykey FROM base b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_dot_product(
            '{1.0, 0.1}'::float4[], b.embedding,
            options => '{"num_leaves_to_search": 1}'::jsonb)
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{2}, {1}}));
  EXPECT_THAT(Query(R"sql(
          SELECT b.mykey FROM base b
          WHERE b.embedding2 IS NOT NULL
          ORDER BY spanner.approx_euclidean_distance(
            b.embedding2, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
              IsOkAndHoldsRows({{1}, {2}}));
  EXPECT_THAT(Query(R"sql(
          SELECT b.mykey, b.mydata FROM base /*@ force_index=vec_index_store */ b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_cosine_distance(
            b.embedding, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves_to_search": 1}')
          LIMIT 1)sql"),
              IsOkAndHoldsRows({{1, "datastr"}}));
}

TEST_F(PGANNTest, ANNQueryWithFloat8Vectors) {
  EXPECT_THAT(QueryWithParams(R"sql(
          SELECT b.mykey FROM base /*@ force_index=vec_index_double */ b
          WHERE b.embedding3 IS NOT NULL
          ORDER BY spanner.approx_cosine_distance(
            $1, b.embedding3, options => '{"num_leaves_to_search": 1}')
          LIMIT 2)sql",
                              {{"p1", Value(std::vector<double>{1.0, 0.1})}}),
              IsOkAndHoldsRows({{1}, {2}}));
}

TEST_F(PGANNTest, ANNQueryErrors) {
  EXPECT_THAT(
      Query(R"sql(
          SELECT b.mykey FROM base /*@ force_index=vec_index */ b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_cosine_distance(
            b.embedding, '{1.0, 0.1}'::float4[])
          LIMIT 2)sql"),
      error::ApproxDistanceFunctionOptionsRequired("approx_cosine_distance"));
  EXPECT_THAT(
      Query(R"sql(
          SELECT b.mykey FROM base /*@ force_index=vec_index */ b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_cosine_distance(
            b.embedding, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves": 1}')
          LIMIT 2)sql"),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("Argument `options` of function "
                         "APPROX_COSINE_DISTANCE is invalid.")));
  EXPECT_THAT(
      Query(R"sql(
          SELECT b.mykey FROM base /*@ force_index=vec_index */ b
          WHERE b.embedding IS NOT NULL
          ORDER BY spanner.approx_euclidean_distance(
            b.embedding, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves_to_search": 1}')
          LIMIT 2)sql"),
      error::VectorIndexesUnusableForceIndexWrongDistanceType(
          "vec_index", "COSINE", "APPROX_EUCLIDEAN_DISTANCE", "embedding"));
  // Without ORDER BY ... LIMIT the functions cannot use a vector index.
  EXPECT_THAT(Query(R"sql(
          SELECT spanner.approx_cosine_distance(
            b.embedding, '{1.0, 0.1}'::float4[],
            options => '{"num_leaves_to_search": 1}')
          FROM base b WHERE b.embedding IS NOT NULL)sql"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("APPROX_COSINE_DISTANCE is not supported")));
}

TEST_F(PGANNTest, VectorIndexDdl) {
  EXPECT_THAT(UpdateSchema({R"sql(
          CREATE INDEX bad_method ON base USING hash (embedding))sql"}),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       HasSubstr("Setting access method is not supported")));
  EXPECT_THAT(UpdateSchema({R"sql(
          CREATE INDEX bad_option ON base USING scann (embedding)
          WITH (distance_type = 'COSINE', bogus = 1))sql"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Invalid vector index option 'bogus'")));
  EXPECT_THAT(UpdateSchema({R"sql(
          CREATE INDEX bad_depth ON base USING scann (embedding)
          WITH (distance_type = 'COSINE', tree_depth = 4))sql"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("tree depth must be 2 or 3")));
  EXPECT_THAT(UpdateSchema({R"sql(
          CREATE INDEX bad_distance ON base USING scann (embedding)
          WITH (distance_type = 'MANHATTAN'))sql"}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("distance_type")));
  GOOGLESQL_EXPECT_OK(UpdateSchema({"DROP INDEX vec_index_store"}));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
