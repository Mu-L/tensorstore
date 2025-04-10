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

#include "tensorstore/internal/http/http_request.h"

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/functional/function_ref.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tensorstore/internal/http/http_header.h"
#include "tensorstore/internal/uri_utils.h"
#include "tensorstore/kvstore/byte_range.h"
#include "tensorstore/util/status.h"

namespace tensorstore {
namespace internal_http {

/// Formats a `range` header to the http request if the byte_range
/// is specified.
std::optional<std::string> FormatRangeHeader(
    OptionalByteRangeRequest byte_range) {
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Range
  assert(byte_range.SatisfiesInvariants());
  if (byte_range.IsRange() &&
      byte_range.exclusive_max > byte_range.inclusive_min) {
    // The range header is inclusive; `Range: 1-0` is invalid.
    return absl::StrFormat("bytes=%d-%d", byte_range.inclusive_min,
                           byte_range.exclusive_max - 1);
  }
  if (byte_range.IsSuffix()) {
    return absl::StrFormat("bytes=%d-", byte_range.inclusive_min);
  }
  if (byte_range.IsSuffixLength()) {
    return absl::StrFormat("bytes=%d", byte_range.inclusive_min);
  }
  return std::nullopt;
}

/// Formats a `cache-control` header specifying `max-age` or `no-cache`.
std::optional<std::string> FormatCacheControlMaxAgeHeader(
    absl::Duration max_age) {
  // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Cache-Control
  if (max_age >= absl::InfiniteDuration()) {
    return std::nullopt;
  }

  // Since max-age is specified as an integer number of seconds, always
  // round down to ensure our requirement is met.
  auto max_age_seconds = absl::ToInt64Seconds(max_age);
  if (max_age_seconds > 0) {
    return absl::StrFormat("max-age=%d", max_age_seconds);
  } else {
    return "no-cache";
  }
}

/// Formats a `cache-control` header consistent with `staleness_bound`.
std::optional<std::string> FormatStalenessBoundCacheControlHeader(
    absl::Time staleness_bound) {
  // `absl::InfiniteFuture()`  indicates that the result must be current.
  // `absl::InfinitePast()`  disables the cache-control header.
  if (staleness_bound == absl::InfinitePast()) {
    return std::nullopt;
  }
  absl::Time now;
  absl::Duration duration = absl::ZeroDuration();
  if (staleness_bound != absl::InfiniteFuture() &&
      (now = absl::Now()) > staleness_bound) {
    // the max age is in the past.
    duration = now - staleness_bound;
  }

  return FormatCacheControlMaxAgeHeader(duration);
}

HttpRequestBuilder::HttpRequestBuilder(
    std::string_view method, std::string base_url,
    absl::FunctionRef<std::string(std::string_view)> uri_encoder)
    : uri_encoder_(uri_encoder),
      request_{std::string(method), std::move(base_url)},
      query_parameter_separator_("?") {
  assert(!request_.method.empty());
  assert(request_.method ==
         absl::AsciiStrToUpper(std::string_view(request_.method)));
  if (request_.url.find_last_of('?') != std::string::npos) {
    query_parameter_separator_ = "&";
  }
}

HttpRequest HttpRequestBuilder::BuildRequest() { return std::move(request_); }

HttpRequestBuilder& HttpRequestBuilder::AddQueryParameter(
    std::string_view key, std::string_view value) {
  assert(!key.empty());

  if (value.empty()) {
    absl::StrAppend(&request_.url, query_parameter_separator_,
                    uri_encoder_(key));
  } else {
    absl::StrAppend(&request_.url, query_parameter_separator_,
                    uri_encoder_(key), "=", uri_encoder_(value));
  }
  query_parameter_separator_ = "&";
  return *this;
}

HttpRequestBuilder& HttpRequestBuilder::EnableAcceptEncoding() {
  request_.accept_encoding = true;
  return *this;
}

HttpRequestBuilder& HttpRequestBuilder::AddHeader(
    std::string_view field_name, std::string_view field_value) {
  auto result = ValidateHttpHeader(field_name, field_value);
  TENSORSTORE_CHECK_OK(result);
  request_.headers.SetHeader(result->first, result->second);
  return *this;
}

HttpRequestBuilder& HttpRequestBuilder::ParseAndAddHeader(
    std::string_view header) {
  auto result = ValidateHttpHeader(header);
  TENSORSTORE_CHECK_OK(result);
  request_.headers.SetHeader(absl::AsciiStrToLower(result->first),
                             result->second);
  return *this;
}

HttpRequestBuilder& HttpRequestBuilder::MaybeAddRangeHeader(
    OptionalByteRangeRequest byte_range) {
  auto field_value = FormatRangeHeader(std::move(byte_range));
  return field_value ? AddHeader("range", *field_value) : *this;
}

HttpRequestBuilder& HttpRequestBuilder::MaybeAddCacheControlMaxAgeHeader(
    absl::Duration max_age) {
  auto field_value = FormatCacheControlMaxAgeHeader(max_age);
  return field_value ? AddHeader("cache-control", *field_value) : *this;
}

HttpRequestBuilder&
HttpRequestBuilder::MaybeAddStalenessBoundCacheControlHeader(
    absl::Time staleness_bound) {
  auto field_value = FormatStalenessBoundCacheControlHeader(staleness_bound);
  return field_value ? AddHeader("cache-control", *field_value) : *this;
}

HttpRequestBuilder& HttpRequestBuilder::AddHostHeader(std::string_view host) {
  if (host.empty()) {
    host = internal::ParseGenericUri(request_.url).authority;
  }
  return AddHeader("host", host);
}

}  // namespace internal_http
}  // namespace tensorstore
