/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/kernels/data/repeat_dataset_op.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "tensorflow/core/data/name_utils.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tsl/platform/errors.h"

namespace tensorflow {
namespace data {

// See documentation in ../../ops/dataset_ops.cc for a high-level
// description of the following op.

/* static */ constexpr const char* const RepeatDatasetOp::kDatasetType;
/* static */ constexpr const char* const RepeatDatasetOp::kInputDataset;
/* static */ constexpr const char* const RepeatDatasetOp::kCount;
/* static */ constexpr const char* const RepeatDatasetOp::kOutputTypes;
/* static */ constexpr const char* const RepeatDatasetOp::kOutputShapes;

namespace {

constexpr char kForeverRepeat[] = "ForeverRepeat";
constexpr char kEmptyRepeat[] = "EmptyRepeat";
constexpr char kFiniteRepeat[] = "FiniteRepeat";
constexpr char kCurIteration[] = "i";
constexpr char kInputImplEmpty[] = "input_impl_empty";
constexpr char kUninitialized[] = "uninitialized";
constexpr int64_t kKnownRatio = 1;

std::string nested_prefix(const std::string& prefix, int64_t epoch) {
  return strings::StrCat(prefix, "[", epoch, "]");
}

// Returns whether `dataset` has an input dataset of the given type. This check
// includes transitive inputs. Returns true if any upstream dataset is a data
// service dataset. Returns false if no upstream dataset is a data service
// dataset, or it's unknown because `dataset` doesn't implement `InputDatasets`.
// TODO(b/269673112): Rewrite the dataset to add an `IsDynamic` attribute to
// signal if the repeated dataset is dynamic or not.
bool HasDataServiceInput(const DatasetBase* dataset) {
  DCHECK(dataset != nullptr);
  if (absl::StartsWith(dataset->type_string(), "DataServiceDataset")) {
    return true;
  }
  std::vector<const DatasetBase*> inputs;
  Status s = dataset->InputDatasets(&inputs);
  if (!s.ok()) {
    return false;
  }
  for (const DatasetBase* input : inputs) {
    if (HasDataServiceInput(input)) {
      return true;
    }
  }
  return false;
}
}  // namespace

// Updates an input split provider with the appropriate cardinality count based
// on how many times it is repeated.
class RepeatedSplitProvider : public SplitProvider {
 public:
  explicit RepeatedSplitProvider(std::unique_ptr<SplitProvider> split_provider,
                                 int64_t count)
      : split_provider_(std::move(split_provider)), count_(count) {}

  // Updates the cardinality based on the times the input dataset is repeated.
  int64_t Cardinality() const override {
    if (split_provider_->Cardinality() == 0 || count_ == 0) {
      return 0;
    }
    // From tensorflow/python/data/ops/repeat_op.py, the repeat op uses -1 for
    // infinite repetitions.
    if (count_ < 0) {
      return kInfiniteCardinality;
    }
    if (split_provider_->Cardinality() < 0) {
      return split_provider_->Cardinality();
    }
    return split_provider_->Cardinality() * count_;
  }

  // The following are the same as the input split provider.
  absl::Status GetNext(Tensor* split, bool* end_of_splits) override {
    return split_provider_->GetNext(split, end_of_splits);
  }
  absl::Status Reset() override { return split_provider_->Reset(); }
  absl::Status Save(std::function<std::string(std::string)> full_name,
                    IteratorStateWriter* writer) override {
    return split_provider_->Save(full_name, writer);
  }
  absl::Status Restore(std::function<std::string(std::string)> full_name,
                       IteratorStateReader* reader) override {
    return split_provider_->Restore(full_name, reader);
  }

 private:
  const std::unique_ptr<SplitProvider> split_provider_;
  const int64_t count_;
};

class RepeatDatasetOp::Dataset : public DatasetBase {
 public:
  Dataset(OpKernelContext* ctx, int64_t count, const DatasetBase* input)
      : DatasetBase(DatasetContext(ctx)), count_(count), input_(input) {
    input_->Ref();
  }

  ~Dataset() override { input_->Unref(); }

  std::unique_ptr<IteratorBase> MakeIteratorInternal(
      const string& prefix) const override {
    if (count_ < 0) {
      return std::make_unique<ForeverIterator>(ForeverIterator::Params{
          this, name_utils::IteratorPrefix(kForeverRepeat, prefix)});
    } else if (count_ == 0) {
      return std::make_unique<EmptyIterator>(EmptyIterator::Params{
          this, name_utils::IteratorPrefix(kEmptyRepeat, prefix)});
    } else {
      return std::make_unique<FiniteIterator>(FiniteIterator::Params{
          this, name_utils::IteratorPrefix(kFiniteRepeat, prefix)});
    }
  }

  absl::Status MakeSplitProviders(std::vector<std::unique_ptr<SplitProvider>>*
                                      split_providers) const override {
    std::vector<std::unique_ptr<SplitProvider>> input_split_providers;
    TF_RETURN_IF_ERROR(input_->MakeSplitProviders(&input_split_providers));

    split_providers->clear();
    for (auto& split_provider : input_split_providers) {
      split_providers->push_back(std::make_unique<RepeatedSplitProvider>(
          std::move(split_provider), count_));
    }
    return absl::OkStatus();
  }

  const DataTypeVector& output_dtypes() const override {
    return input_->output_dtypes();
  }
  const std::vector<PartialTensorShape>& output_shapes() const override {
    return input_->output_shapes();
  }

  string DebugString() const override {
    return name_utils::DatasetDebugString(RepeatDatasetOp::kDatasetType);
  }

  int64_t CardinalityInternal(CardinalityOptions options) const override {
    int64_t n = input_->Cardinality(options);
    if (count_ < 0) {
      if (n == 0) {
        return 0;
      }
      return kInfiniteCardinality;
    }
    if (count_ == 0) {
      return 0;
    }
    if (n == kInfiniteCardinality || n == kUnknownCardinality) {
      return n;
    }
    return count_ * n;
  }

  Status InputDatasets(std::vector<const DatasetBase*>* inputs) const override {
    inputs->push_back(input_);
    return OkStatus();
  }

  Status CheckExternalState() const override {
    return input_->CheckExternalState();
  }

  Status Get(OpKernelContext* ctx, int64 index,
             std::vector<Tensor>* out_tensors) const override {
    TF_RETURN_IF_ERROR(CheckRandomAccessCompatible(index));
    return input_->Get(ctx, index % input_->Cardinality(), out_tensors);
  }

 protected:
  Status AsGraphDefInternal(SerializationContext* ctx,
                            DatasetGraphDefBuilder* b,
                            Node** output) const override {
    Node* input_graph_node = nullptr;
    TF_RETURN_IF_ERROR(b->AddInputDataset(ctx, input_, &input_graph_node));
    Node* count = nullptr;
    TF_RETURN_IF_ERROR(b->AddScalar(count_, &count));
    TF_RETURN_IF_ERROR(b->AddDataset(this, {input_graph_node, count}, output));
    return OkStatus();
  }

 private:
  class EmptyIterator : public DatasetIterator<Dataset> {
   public:
    explicit EmptyIterator(const Params& params)
        : DatasetIterator<Dataset>(params) {}

    bool SymbolicCheckpointCompatible() const override { return true; }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      *end_of_sequence = true;
      return OkStatus();
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeKnownRatioNode(std::move(args),
                                       /*ratio=*/kKnownRatio);
    }

    Status SaveInternal(SerializationContext* ctx,
                        IteratorStateWriter* writer) override {
      return OkStatus();
    }
    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      return OkStatus();
    }
  };

  class FiniteIterator : public DatasetIterator<Dataset> {
   public:
    explicit FiniteIterator(const Params& params)
        : DatasetIterator<Dataset>(params), i_(0) {}

    bool SymbolicCheckpointCompatible() const override { return true; }

    Status Initialize(IteratorContext* ctx) override {
      mutex_lock l(mu_);
      return dataset()->input_->MakeIterator(
          ctx, this, nested_prefix(prefix(), i_), &input_impl_);
    }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      mutex_lock l(mu_);  // TODO(mrry): Make locking less conservative.
      if (!input_impl_) {
        *end_of_sequence = true;
        return OkStatus();
      }
      while (i_ < dataset()->count_) {
        TF_RETURN_IF_ERROR(
            input_impl_->GetNext(ctx, out_tensors, end_of_sequence));
        if (!*end_of_sequence) {
          return OkStatus();
        }
        ctx->PurgeCheckpoint(nested_prefix(prefix(), i_));
        ++i_;
        input_impl_.reset();
        for (const auto& provider : ctx->split_providers()) {
          TF_RETURN_IF_ERROR(provider->Reset());
        }
        TF_RETURN_IF_ERROR(dataset()->input_->MakeIterator(
            ctx, this, nested_prefix(prefix(), i_), &input_impl_));
      }
      *end_of_sequence = true;
      input_impl_.reset();
      return OkStatus();
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeKnownRatioNode(std::move(args),
                                       /*ratio=*/kKnownRatio);
    }

    Status SaveInternal(SerializationContext* ctx,
                        IteratorStateWriter* writer) override {
      mutex_lock l(mu_);
      TF_RETURN_IF_ERROR(writer->WriteScalar(prefix(), kCurIteration, i_));
      TF_RETURN_IF_ERROR(writer->WriteScalar(
          prefix(), kInputImplEmpty, static_cast<int64_t>(!input_impl_)));
      if (input_impl_) {
        TF_RETURN_IF_ERROR(SaveInput(ctx, writer, input_impl_));
      }
      return OkStatus();
    }

    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      mutex_lock l(mu_);
      TF_RETURN_IF_ERROR(reader->ReadScalar(prefix(), kCurIteration, &i_));
      int64_t input_empty;
      TF_RETURN_IF_ERROR(
          reader->ReadScalar(prefix(), kInputImplEmpty, &input_empty));
      if (static_cast<bool>(!input_empty)) {
        TF_RETURN_IF_ERROR(dataset()->input_->MakeIterator(
            ctx, this, nested_prefix(prefix(), i_), &input_impl_));
        TF_RETURN_IF_ERROR(RestoreInput(ctx, reader, input_impl_));
      } else {
        input_impl_.reset();
      }
      return OkStatus();
    }

   private:
    mutex mu_;
    int64_t i_ TF_GUARDED_BY(mu_);
    std::unique_ptr<IteratorBase> input_impl_ TF_GUARDED_BY(mu_);
  };

  class ForeverIterator : public DatasetIterator<Dataset> {
   public:
    explicit ForeverIterator(const Params& params)
        : DatasetIterator<Dataset>(params),
          has_data_service_input_(HasDataServiceInput(dataset())),
          input_impl_(nullptr),
          i_(0),
          first_call_(true) {}

    bool SymbolicCheckpointCompatible() const override { return true; }

    Status Initialize(IteratorContext* ctx) override {
      mutex_lock l(mu_);
      return dataset()->input_->MakeIterator(
          ctx, this, nested_prefix(prefix(), i_), &input_impl_);
    }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      mutex_lock l(mu_);  // TODO(mrry): Make locking less conservative.
      do {
        if (!input_impl_) {
          TF_RETURN_IF_ERROR(dataset()->input_->MakeIterator(
              ctx, this, nested_prefix(prefix(), i_), &input_impl_));
        }
        TF_RETURN_IF_ERROR(
            input_impl_->GetNext(ctx, out_tensors, end_of_sequence));
        DCHECK(!*end_of_sequence || out_tensors->empty());
        if (first_call_ && *end_of_sequence && ctx->split_providers().empty()) {
          // If the first call to GetNext() fails because the end of sequence
          // has been reached, we return EOF unless it repeats a tf.data service
          // dataset, where the repeated elements are non-deterministic.
          // Otherwise, this iterator could loop infinitely.
          if (!has_data_service_input_) {
            input_impl_.reset();
            return OkStatus();
          }
        }
        first_call_ = false;
        if (!*end_of_sequence) {
          return OkStatus();
        }
        ctx->PurgeCheckpoint(nested_prefix(prefix(), i_));
        ++i_;
        for (const auto& provider : ctx->split_providers()) {
          TF_RETURN_IF_ERROR(provider->Reset());
        }
        input_impl_.reset();
        first_call_ = true;
      } while (true);
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeKnownRatioNode(std::move(args),
                                       /*ratio=*/kKnownRatio);
    }

    Status SaveInternal(SerializationContext* ctx,
                        IteratorStateWriter* writer) override {
      mutex_lock l(mu_);
      TF_RETURN_IF_ERROR(writer->WriteScalar(prefix(), kCurIteration, i_));
      TF_RETURN_IF_ERROR(writer->WriteScalar(
          prefix(), kInputImplEmpty, static_cast<int64_t>(!input_impl_)));
      if (input_impl_) {
        TF_RETURN_IF_ERROR(SaveInput(ctx, writer, input_impl_));
      }
      return OkStatus();
    }

    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      mutex_lock l(mu_);
      TF_RETURN_IF_ERROR(reader->ReadScalar(prefix(), kCurIteration, &i_));
      int64_t input_empty;
      TF_RETURN_IF_ERROR(
          reader->ReadScalar(prefix(), kInputImplEmpty, &input_empty));
      if (static_cast<bool>(input_empty)) {
        input_impl_.reset();
        first_call_ = true;
      } else {
        TF_RETURN_IF_ERROR(dataset()->input_->MakeIterator(
            ctx, this, nested_prefix(prefix(), i_), &input_impl_));
        TF_RETURN_IF_ERROR(RestoreInput(ctx, reader, input_impl_));
        first_call_ = false;
      }
      return OkStatus();
    }

   private:
    const bool has_data_service_input_;

    mutex mu_;
    std::unique_ptr<IteratorBase> input_impl_ TF_GUARDED_BY(mu_);
    int64_t i_ TF_GUARDED_BY(mu_);
    bool first_call_ TF_GUARDED_BY(mu_);
  };

  const int64_t count_;
  const DatasetBase* const input_;
};

RepeatDatasetOp::RepeatDatasetOp(OpKernelConstruction* ctx)
    : UnaryDatasetOpKernel(ctx) {}

void RepeatDatasetOp::MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                                  DatasetBase** output) {
  // Create a new RepeatDatasetOp::Dataset, insert it in the step-local
  // container, and return it as the output.
  int64_t count;
  OP_REQUIRES_OK(ctx, ParseScalarArgument<int64_t>(ctx, kCount, &count));
  *output = new Dataset(ctx, count, input);
}

namespace {
REGISTER_KERNEL_BUILDER(Name("RepeatDataset").Device(DEVICE_CPU),
                        RepeatDatasetOp);
}  // namespace
}  // namespace data
}  // namespace tensorflow
