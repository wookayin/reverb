// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "reverb/cc/structured_writer.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "reverb/cc/chunker.h"
#include "reverb/cc/patterns.pb.h"
#include "reverb/cc/platform/status_macros.h"
#include "reverb/cc/platform/status_matchers.h"
#include "reverb/cc/support/signature.h"
#include "reverb/cc/testing/proto_test_util.h"
#include "reverb/cc/testing/tensor_testutil.h"
#include "reverb/cc/trajectory_writer.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_util.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/platform/types.h"

namespace deepmind::reverb {
namespace {

using ::tensorflow::Tensor;

MATCHER_P2(StatusIs, code, message, "") {
  return arg.code() == code && absl::StrContains(arg.message(), message);
}

inline StructuredWriterConfig MakeConfig(const std::string& text_proto) {
  return testing::ParseTextProtoOrDie<StructuredWriterConfig>(text_proto);
}

inline int Get(const Tensor& tensor, int index) {
  return tensor.flat<tensorflow::int32>().data()[index];
}

inline void Set(Tensor& tensor, int index, tensorflow::int32 value) {
  tensor.flat<tensorflow::int32>().data()[index] = value;
}

class FakeWriter : public ColumnWriter {
 public:
  explicit FakeWriter(int num_columns) {
    for (int i = 0; i < num_columns; i++) {
      chunkers_.push_back(std::make_shared<Chunker>(
          internal::TensorSpec{"", tensorflow::DT_INT32, {}},
          std::make_shared<ConstantChunkerOptions>(1, 100)));
    }
  }

  absl::Status Append(
      std::vector<absl::optional<Tensor>> data,
      std::vector<absl::optional<std::weak_ptr<CellRef>>>* refs) override {
    AppendInternal(std::move(data), refs);
    current_step_.step++;
    return absl::OkStatus();
  }

  absl::Status AppendPartial(
      std::vector<absl::optional<Tensor>> data,
      std::vector<absl::optional<std::weak_ptr<CellRef>>>* refs) override {
    AppendInternal(std::move(data), refs);
    return absl::OkStatus();
  }

  absl::Status CreateItem(
      absl::string_view table, double priority,
      absl::Span<const TrajectoryColumn> trajectory) override {
    std::vector<Tensor> columns;

    for (const auto& trajectory_column : trajectory) {
      std::vector<std::shared_ptr<CellRef>> refs;
      REVERB_CHECK(trajectory_column.LockReferences(&refs));

      tensorflow::TensorShape shape;
      if (!trajectory_column.squeezed()) {
        shape.InsertDim(0, refs.size());
      }
      columns.emplace_back(tensorflow::DT_INT32, shape);

      for (int i = 0; i < refs.size(); i++) {
        Tensor tensor;
        REVERB_CHECK_OK(refs[i]->GetData(&tensor));
        Set(columns.back(), i, Get(tensor, 0));
      }
    }

    trajectories_.push_back(std::move(columns));

    return absl::OkStatus();
  }

  absl::Status EndEpisode(
      bool clear_buffers,
      absl::Duration timeout = absl::InfiniteDuration()) override {
    if (clear_buffers) {
      for (auto& chunker : chunkers_) {
        chunker->Reset();
      }
    }
    current_step_ = {current_step_.episode_id + 1, 0};
    return absl::OkStatus();
  }

  absl::Status Flush(
      int ignore_last_num_items = 0,
      absl::Duration timeout = absl::InfiniteDuration()) override {
    return absl::OkStatus();
  }

  const std::vector<std::vector<Tensor>>& GetWritten() const {
    return trajectories_;
  }

 private:
  void AppendInternal(
      std::vector<absl::optional<Tensor>> data,
      std::vector<absl::optional<std::weak_ptr<CellRef>>>* refs) {
    REVERB_CHECK_LE(data.size(), chunkers_.size());

    for (int i = 0; i < data.size(); i++) {
      if (data[i].has_value()) {
        std::weak_ptr<CellRef> ref;
        REVERB_CHECK_OK(chunkers_[i]->Append(*data[i], current_step_, &ref));
        refs->push_back(std::move(ref));
      } else {
        refs->push_back(absl::nullopt);
      }
    }
  }

  std::vector<std::shared_ptr<Chunker>> chunkers_;
  CellRef::EpisodeInfo current_step_ = {0, 0};
  std::vector<std::vector<Tensor>> trajectories_;
};

Tensor MakeTensor(std::vector<int> values) {
  Tensor tensor(tensorflow::DT_INT32, {static_cast<int>(values.size())});
  for (int i = 0; i < values.size(); i++) {
    Set(tensor, i, values[i]);
  }
  return tensor;
}

Tensor MakeTensor(int value) {
  return Tensor(static_cast<tensorflow::int32>(value));
}

std::vector<absl::optional<Tensor>> MakeStep(
    std::vector<absl::optional<int>> values) {
  std::vector<absl::optional<Tensor>> step;
  for (int i = 0; i < values.size(); i++) {
    if (values[i].has_value()) {
      step.push_back(MakeTensor(values[i].value()));
    } else {
      step.push_back(absl::nullopt);
    }
  }
  return step;
}

void ExpectTrajectoryEqual(const std::vector<Tensor>& want,
                           const std::vector<Tensor>& got) {
  ASSERT_EQ(want.size(), got.size()) << "Wrong number of columns";
  for (int i = 0; i < want.size(); i++) {
    test::ExpectTensorEqual<tensorflow::int32>(got[i], want[i]);
  }
}

void ExpectTrajectoriesEqual(const std::vector<std::vector<Tensor>>& want,
                             const std::vector<std::vector<Tensor>>& got) {
  ASSERT_EQ(want.size(), got.size()) << "Wrong number of trajectories";
  for (int i = 0; i < want.size(); i++) {
    ExpectTrajectoryEqual(want[i], got[i]);
  }
}

TEST(ValidateStructuredWriterConfig, Valid_NoStart) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(
      R"pb(
        flat { flat_source_index: 0 stop: -1 }
        table: "table"
        priority: 1.0
        conditions { buffer_length: true ge: 1 }
      )pb")));
}

TEST(ValidateStructuredWriterConfig, Valid_WithStartAndStop) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(
      R"pb(
        flat { flat_source_index: 0 start: -2 stop: -1 }
        table: "table"
        priority: 1.0
        conditions { buffer_length: true ge: 2 }
      )pb")));
}

TEST(ValidateStructuredWriterConfig, Valid_WithStartAndNoStop) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(
      R"pb(
        flat { flat_source_index: 0 start: -2 }
        table: "table"
        priority: 1.0
        conditions { buffer_length: true ge: 2 }
      )pb")));
}

TEST(ValidateStructuredWriterConfig, NoStartAndNoStop) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(
          R"pb(
            flat { flat_source_index: 0 }
            table: "table"
            priority: 1.0
          )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "At least one of `start` and `stop` must be specified."));
}

TEST(ValidateStructuredWriterConfig, NegativeFlatSourceIndex) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(
                  R"pb(
                    flat { flat_source_index: -1 }
                    table: "table"
                    priority: 1.0
                  )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`flat_source_index` must be >= 0 but got -1."));
}

TEST(ValidateStructuredWriterConfig, ZeroStart) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 start: 0 }
                table: "table"
                priority: 1.0
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`start` must be < 0 but got 0."));
}

TEST(ValidateStructuredWriterConfig, PositiveStart) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 start: 1 }
                table: "table"
                priority: 1.0
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`start` must be < 0 but got 1."));
}

TEST(ValidateStructuredWriterConfig, PositiveStop) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 start: -1 stop: 1 }
                table: "table"
                priority: 1.0
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`stop` must be <= 0 but got 1."));
}

TEST(ValidateStructuredWriterConfig, StopEqualToStart) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(R"pb(
        flat { flat_source_index: 0 start: -2 stop: -2 }
        table: "table"
        priority: 1.0
      )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "`stop` (-2) must be > `start` (-2) when both are specified."));
}

TEST(ValidateStructuredWriterConfig, StopLessThanStart) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(R"pb(
        flat { flat_source_index: 0 start: -2 stop: -3 }
        table: "table"
        priority: 1.0
      )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "`stop` (-3) must be > `start` (-2) when both are specified."));
}

TEST(ValidateStructuredWriterConfig, ZeroStopAndNoStart) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: 0 }
                table: "table"
                priority: 1.0
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`stop` must be < 0 when `start` isn't set but got 0."));
}

TEST(ValidateStructuredWriterConfig, NoBufferLengthCondition) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(R"pb(
        flat { flat_source_index: 0 stop: -1 }
        table: "table"
        priority: 1.0
      )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Config does not contain required buffer length condition;"));
}

TEST(ValidateStructuredWriterConfig, TooSmallBufferLengthCondition_SingleNode) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(R"pb(
        flat { flat_source_index: 0 stop: -2 }
        table: "table"
        priority: 1.0
        conditions { buffer_length: true ge: 1 }
      )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Config does not contain required buffer length condition;"));
}

TEST(ValidateStructuredWriterConfig, TooSmallBufferLengthCondition_MultiNode) {
  EXPECT_THAT(
      ValidateStructuredWriterConfig(MakeConfig(R"pb(
        flat { flat_source_index: 0 stop: -2 }
        flat { flat_source_index: 0 start: -3 }
        table: "table"
        priority: 1.0
        conditions { buffer_length: true ge: 2 }
      )pb")),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Config does not contain required buffer length condition;"));
}

TEST(ValidateStructuredWriterConfig, Valid_TooLargeBufferLength_SingleNode) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(R"pb(
    flat { flat_source_index: 0 stop: -2 }
    table: "table"
    priority: 1.0
    conditions { buffer_length: true ge: 3 }
  )pb")));
}

TEST(ValidateStructuredWriterConfig, Valid_TooLargeBufferLength_MultiNode) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(R"pb(
    flat { flat_source_index: 0 stop: -2 }
    flat { flat_source_index: 0 stop: -1 }
    table: "table"
    priority: 1.0
    conditions { buffer_length: true ge: 3 }
  )pb")));
}

TEST(ValidateStructuredWriterConfig, NoLeftInCondition) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: 1.0
                conditions { ge: 2 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "Conditions must specify a value for `left`"));
}

TEST(ValidateStructuredWriterConfig, NegativeModuloInCondition) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: 1.0
                conditions {
                  step_index: true
                  mod_eq { mod: -2 eq: 0 }
                }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`mod_eq.mod` must be > 0 but got -2."));
}

TEST(ValidateStructuredWriterConfig, ZeroModuloInCondition) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: 1.0
                conditions {
                  step_index: true
                  mod_eq { mod: 0 eq: 0 }
                }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`mod_eq.mod` must be > 0 but got 0."));
}

TEST(ValidateStructuredWriterConfig, NegativeModuloEqInCondition) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: 1.0
                conditions {
                  step_index: true
                  mod_eq { mod: 2 eq: -1 }
                }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`mod_eq.eq` must be >= 0 but got -1."));
}

TEST(ValidateStructuredWriterConfig, NoCmpInCondition) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: 1.0
                conditions { step_index: true }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "Conditions must specify a value for `cmp`."));
}

TEST(ValidateStructuredWriterConfig, Valid_EndOfEpisodeCondition) {
  REVERB_EXPECT_OK(ValidateStructuredWriterConfig(MakeConfig(R"pb(
    flat { flat_source_index: 0 stop: -2 }
    table: "table"
    priority: 1.0
    conditions { buffer_length: true ge: 2 }
    conditions { is_end_episode: true eq: 1 }
  )pb")));
}

TEST(ValidateStructuredWriterConfig, EndOfEpisode_NotUsingEqOne) {
  auto valid = MakeConfig(R"pb(
    flat { flat_source_index: 0 stop: -2 }
    table: "table"
    priority: 1.0
    conditions { buffer_length: true ge: 2 }
    conditions { is_end_episode: true eq: 1 }
  )pb");
  REVERB_ASSERT_OK(ValidateStructuredWriterConfig(valid));

  StructuredWriterConfig ge = valid;
  ge.mutable_conditions(1)->set_ge(1);
  EXPECT_THAT(
      ValidateStructuredWriterConfig(ge),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Condition must use `eq=1` when using `is_end_episode`"));

  StructuredWriterConfig eq_zero = valid;
  eq_zero.mutable_conditions(1)->set_eq(0);
  EXPECT_THAT(
      ValidateStructuredWriterConfig(eq_zero),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Condition must use `eq=1` when using `is_end_episode`"));

  StructuredWriterConfig eq_two = valid;
  eq_two.mutable_conditions(1)->set_eq(2);
  EXPECT_THAT(
      ValidateStructuredWriterConfig(eq_two),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Condition must use `eq=1` when using `is_end_episode`"));

  StructuredWriterConfig le = valid;
  le.mutable_conditions(1)->set_le(1);
  EXPECT_THAT(
      ValidateStructuredWriterConfig(le),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "Condition must use `eq=1` when using `is_end_episode`"));
}

TEST(ValidateStructuredWriterConfig, FlatIsEmpty) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                table: "table"
                priority: 1.0
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`flat` must not be empty."));
}

TEST(ValidateStructuredWriterConfig, TableIsEmpty) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                priority: 1.0
                conditions { buffer_length: true ge: 2 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`table` must not be empty."));
}

TEST(ValidateStructuredWriterConfig, NegativePriority) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -2 }
                table: "table"
                priority: -1.0
                conditions { buffer_length: true ge: 2 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`priority` must be >= 0 but got -1.0"));
}

TEST(ValidateStructuredWriterConfig, StepSetWhenStartUnset) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 stop: -3 step: 2 }
                table: "table"
                priority: 1.0
                conditions { buffer_length: true ge: 3 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`step` must only be set when `start` is set."));
}

TEST(ValidateStructuredWriterConfig, NegativeStep) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 start: -3 step: -1 }
                table: "table"
                priority: 1.0
                conditions { buffer_length: true ge: 3 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`step` must be > 0 but got -1."));
}

TEST(ValidateStructuredWriterConfig, ZeroStep) {
  EXPECT_THAT(ValidateStructuredWriterConfig(MakeConfig(R"pb(
                flat { flat_source_index: 0 start: -3 step: 0 }
                table: "table"
                priority: 1.0
                conditions { buffer_length: true ge: 3 }
              )pb")),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "`step` must be > 0 but got 0."));
}

TEST(StructuredWriter, PatternFromPartialData) {
  auto fake_writer = absl::make_unique<FakeWriter>(2);
  FakeWriter* fake_writer_ptr = fake_writer.get();

  auto config = MakeConfig(R"pb(
    flat { flat_source_index: 0 stop: -1 }
    flat { flat_source_index: 1 start: -2 }
    table: "table"
    priority: 1.0
    conditions: { buffer_length: true ge: 2 }
  )pb");

  StructuredWriter writer(std::move(fake_writer), {std::move(config)});

  REVERB_EXPECT_OK(writer.Append(MakeStep({10, 20})));
  REVERB_EXPECT_OK(writer.Append(MakeStep({absl::nullopt, 21})));
  REVERB_EXPECT_OK(writer.Append(MakeStep({12, 22})));
  REVERB_EXPECT_OK(writer.Append(MakeStep({absl::nullopt, 23})));
  REVERB_EXPECT_OK(writer.Append(MakeStep({14, 24})));

  ExpectTrajectoriesEqual(fake_writer_ptr->GetWritten(),
                          {
                              {MakeTensor(12), MakeTensor({21, 22})},
                              {MakeTensor(14), MakeTensor({23, 24})},
                          });
}

using ParamT = std::pair<std::string, std::vector<std::vector<Tensor>>>;

class StructuredWriterTest : public ::testing::TestWithParam<ParamT> {};

TEST_P(StructuredWriterTest, AppliesPattern) {
  auto fake_writer = absl::make_unique<FakeWriter>(3);
  FakeWriter* fake_writer_ptr = fake_writer.get();

  auto params = GetParam();
  auto config = MakeConfig(params.first);
  config.set_table("table");
  config.set_priority(1.0);

  auto it = std::max_element(config.flat().begin(), config.flat().end(),
                             [](const auto& a, const auto& b) {
                               return std::abs(std::min(a.start(), a.stop())) <
                                      std::abs(std::min(b.start(), b.stop()));
                             });
  auto* condition = config.add_conditions();
  condition->set_buffer_length(true);
  condition->set_ge(std::abs(std::min(it->start(), it->stop())));

  StructuredWriter writer(std::move(fake_writer), {std::move(config)});

  for (int i = 0; i < 5; i++) {
    REVERB_EXPECT_OK(writer.Append(MakeStep({10 + i, 20 + i, 30 + i})));
  }
  REVERB_EXPECT_OK(writer.EndEpisode(/*clear_buffers=*/true));

  ExpectTrajectoriesEqual(fake_writer_ptr->GetWritten(), params.second);
}

INSTANTIATE_TEST_SUITE_P(
    SelectSingleSqueezed, StructuredWriterTest,
    ::testing::Values(ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                          )pb",
                          {
                              {MakeTensor(10)},
                              {MakeTensor(11)},
                              {MakeTensor(12)},
                              {MakeTensor(13)},
                              {MakeTensor(14)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 2 stop: -2 }
                          )pb",
                          {
                              {MakeTensor(30)},
                              {MakeTensor(31)},
                              {MakeTensor(32)},
                              {MakeTensor(33)},
                          })));

INSTANTIATE_TEST_SUITE_P(
    SingleSlice, StructuredWriterTest,
    ::testing::Values(ParamT(
                          R"pb(
                            flat { flat_source_index: 1 start: -2 }
                          )pb",
                          {
                              {MakeTensor({20, 21})},
                              {MakeTensor({21, 22})},
                              {MakeTensor({22, 23})},
                              {MakeTensor({23, 24})},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 2 start: -3 stop: -1 }
                          )pb",
                          {
                              {MakeTensor({30, 31})},
                              {MakeTensor({31, 32})},
                              {MakeTensor({32, 33})},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 2 start: -3 stop: -2 }
                          )pb",
                          {
                              {MakeTensor(std::vector<int>{30})},
                              {MakeTensor(std::vector<int>{31})},
                              {MakeTensor(std::vector<int>{32})},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 start: -3 }
                          )pb",
                          {
                              {MakeTensor({10, 11, 12})},
                              {MakeTensor({11, 12, 13})},
                              {MakeTensor({12, 13, 14})},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 start: -3 step: 2 }
                          )pb",
                          {
                              {MakeTensor({10, 12})},
                              {MakeTensor({11, 13})},
                              {MakeTensor({12, 14})},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 1 start: -4 step: 3 }
                          )pb",
                          {
                              {MakeTensor({20, 23})},
                              {MakeTensor({21, 24})},
                          })));

INSTANTIATE_TEST_SUITE_P(
    SliceAndSqueeze, StructuredWriterTest,
    ::testing::Values(
        ParamT(
            R"pb(
              flat { flat_source_index: 0 stop: -1 }
              flat { flat_source_index: 1 start: -1 stop: 0 }
            )pb",
            {
                {MakeTensor(10), MakeTensor(std::vector<int>{20})},
                {MakeTensor(11), MakeTensor(std::vector<int>{21})},
                {MakeTensor(12), MakeTensor(std::vector<int>{22})},
                {MakeTensor(13), MakeTensor(std::vector<int>{23})},
                {MakeTensor(14), MakeTensor(std::vector<int>{24})},
            }),
        ParamT(
            R"pb(
              flat { flat_source_index: 2 start: -3 stop: -1 }
              flat { flat_source_index: 0 stop: -2 }
            )pb",
            {
                {MakeTensor({30, 31}), MakeTensor(11)},
                {MakeTensor({31, 32}), MakeTensor(12)},
                {MakeTensor({32, 33}), MakeTensor(13)},
            })));

INSTANTIATE_TEST_SUITE_P(
    StepIndexCondition, StructuredWriterTest,
    ::testing::Values(ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions {
                              step_index: true
                              mod_eq { mod: 2 eq: 0 }
                            }
                          )pb",
                          {
                              {MakeTensor(10)},
                              {MakeTensor(12)},
                              {MakeTensor(14)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions {
                              step_index: true
                              mod_eq { mod: 3 eq: 1 }
                            }
                          )pb",
                          {
                              {MakeTensor(11)},
                              {MakeTensor(14)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions { step_index: true eq: 2 }
                          )pb",
                          {
                              {MakeTensor(12)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions { step_index: true ge: 2 }
                          )pb",
                          {
                              {MakeTensor(12)},
                              {MakeTensor(13)},
                              {MakeTensor(14)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions { step_index: true le: 2 }
                          )pb",
                          {
                              {MakeTensor(10)},
                              {MakeTensor(11)},
                              {MakeTensor(12)},
                          })));

INSTANTIATE_TEST_SUITE_P(
    StepsSinceAppliedCondition, StructuredWriterTest,
    ::testing::Values(ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions { steps_since_applied: true ge: 2 }
                          )pb",
                          {
                              {MakeTensor(11)},
                              {MakeTensor(13)},
                          }),
                      ParamT(
                          R"pb(
                            flat { flat_source_index: 0 stop: -1 }
                            conditions { steps_since_applied: true ge: 3 }
                          )pb",
                          {
                              {MakeTensor(12)},
                          })));

INSTANTIATE_TEST_SUITE_P(EndOfEpisodeCondition, StructuredWriterTest,
                         ::testing::Values(ParamT(
                             R"pb(
                               flat { flat_source_index: 0 stop: -1 }
                               conditions { is_end_episode: true eq: 1 }
                             )pb",
                             {
                                 {MakeTensor(14)},
                             })));
}  // namespace
}  // namespace deepmind::reverb
