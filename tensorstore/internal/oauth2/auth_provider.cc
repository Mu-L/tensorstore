// Copyright 2020 The TensorStore Authors
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

#include "tensorstore/internal/oauth2/auth_provider.h"

#include <string>

#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/str_cat.h"

namespace tensorstore {
namespace internal_oauth2 {

AuthProvider::~AuthProvider() = default;

Result<std::string> AuthProvider::GetAuthHeader() {
  auto token = this->GetToken();
  TENSORSTORE_RETURN_IF_ERROR(token);
  return tensorstore::StrCat("Authorization: Bearer ", token->token);
}

}  // namespace internal_oauth2
}  // namespace tensorstore
