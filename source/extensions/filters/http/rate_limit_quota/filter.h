#pragma once
#include <memory>

#include "envoy/extensions/filters/http/rate_limit_quota/v3/rate_limit_quota.pb.h"
#include "envoy/extensions/filters/http/rate_limit_quota/v3/rate_limit_quota.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/service/rate_limit_quota/v3/rlqs.pb.h"
#include "envoy/service/rate_limit_quota/v3/rlqs.pb.validate.h"

#include "source/common/http/matching/data_impl.h"
#include "source/common/http/message_impl.h"
#include "source/common/matcher/matcher.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/rate_limit_quota/client.h"
#include "source/extensions/filters/http/rate_limit_quota/client_impl.h"
#include "source/extensions/filters/http/rate_limit_quota/matcher.h"
#include "source/extensions/filters/http/rate_limit_quota/quota_bucket.h"

#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitQuota {

using ::envoy::extensions::filters::http::rate_limit_quota::v3::RateLimitQuotaBucketSettings;
using ::envoy::service::rate_limit_quota::v3::BucketId;
using ::envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports;
using FilterConfig =
    envoy::extensions::filters::http::rate_limit_quota::v3::RateLimitQuotaFilterConfig;
using FilterConfigConstSharedPtr = std::shared_ptr<const FilterConfig>;
using QuotaAssignmentAction = ::envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse::
    BucketAction::QuotaAssignmentAction;

/**
 * Possible async results for a limit call.
 */
enum class RateLimitStatus {
  // The request is not over limit.
  OK,
  // The request is over limit.
  OverLimit
  // // The rate limit service could not be queried.
  // Error,
};

class RateLimitQuotaFilter : public Http::PassThroughFilter,
                             public RateLimitQuotaCallbacks,
                             public Logger::Loggable<Logger::Id::filter> {
public:
  RateLimitQuotaFilter(FilterConfigConstSharedPtr config,
                       Server::Configuration::FactoryContext& factory_context,
                       // TODO(tyxia) Removed the default argument and add const???!!
                       BucketContainer* quota_bucket = nullptr,
                       RateLimitQuotaUsageReports* quota_usage_reports = nullptr)
      : config_(std::move(config)), factory_context_(factory_context), quota_bucket_(quota_bucket),
        quota_usage_reports_(quota_usage_reports) {
    createMatcher();
  }

  // Http::PassThroughDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap&, bool) override;
  void onDestroy() override {}
  // TODO(tyxia) How to close the stream for each quota bucket separately.
  // rate_limit_client_->closeStream(); };
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    callbacks_ = &callbacks;
  }

  // RateLimitQuota::RateLimitQuotaCallbacks
  void onQuotaResponse(envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse&) override {}

  // Perform request matching. It returns the generated bucket ids if the matching succeeded,
  // error status otherwise.
  absl::StatusOr<Matcher::ActionPtr> requestMatching(const Http::RequestHeaderMap& headers);

  Http::Matching::HttpMatchingDataImpl matchingData() {
    ASSERT(data_ptr_ != nullptr);
    return *data_ptr_;
  }

  void onComplete(const RateLimitQuotaBucketSettings& bucket_settings, RateLimitStatus status);
  ~RateLimitQuotaFilter() override = default;

private:
  // Create the matcher factory and matcher.
  void createMatcher();

  FilterConfigConstSharedPtr config_;
  Server::Configuration::FactoryContext& factory_context_;
  Http::StreamDecoderFilterCallbacks* callbacks_ = nullptr;
  RateLimitQuotaValidationVisitor visitor_ = {};
  Matcher::MatchTreeSharedPtr<Http::HttpMatchingData> matcher_ = nullptr;
  std::unique_ptr<Http::Matching::HttpMatchingDataImpl> data_ptr_ = nullptr;
  // TODO(tyxia) This is the threadlocal cache that is created in the main thread.
  BucketContainer* quota_bucket_ = nullptr;
  // TODO(tyxia) Pass in another thread local storage object as well
  // or we wrapp those two objects in a single object
  // This also  needs to be avaible in client_impl.h
  // pass by reference to indicate no ownership transfer
  // TODO(tyxia) const!!!!
  // UsageReportsContainer* const usage_reports_;
  // go/totw/177#reference-members
  // UsageReportsContainer& usage_report_ = nullptr;
  RateLimitQuotaUsageReports* quota_usage_reports_ = nullptr;
};

} // namespace RateLimitQuota
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
