// Copyright 2025 The TensorStore Authors
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

#ifndef TENSORSTORE_INTERNAL_TRACING_OPERATION_TRACE_SPAN_H_
#define TENSORSTORE_INTERNAL_TRACING_OPERATION_TRACE_SPAN_H_

#include <stddef.h>
#include <stdint.h>

#include <string_view>

#include "tensorstore/internal/source_location.h"

namespace tensorstore {
namespace internal_tracing {

/// An OperationTraceSpan is an operation trace annotation used to tag
/// asynchronous operations which may be long running.
class OperationTraceSpan {
 public:
  OperationTraceSpan(std::string_view method,
                     const SourceLocation& location = SourceLocation::current())
  {}

  ~OperationTraceSpan() = default;

 private:
};

}  // namespace internal_tracing
}  // namespace tensorstore

#endif  // TENSORSTORE_INTERNAL_TRACING_OPERATION_TRACE_SPAN_H_
