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

#ifndef TENSORSTORE_INTERNAL_HTTP_SELF_SIGNED_CERT_H_
#define TENSORSTORE_INTERNAL_HTTP_SELF_SIGNED_CERT_H_

#include <string>

#include "tensorstore/util/result.h"

namespace tensorstore {
namespace internal_http {

// A self-signed certificate and private key.
struct SelfSignedCertificate {
  std::string key_pem;
  std::string cert_pem;
};
Result<SelfSignedCertificate> GenerateSelfSignedCerts();

}  // namespace internal_http
}  // namespace tensorstore

#endif  // TENSORSTORE_INTERNAL_HTTP_SELF_SIGNED_CERT_H_
