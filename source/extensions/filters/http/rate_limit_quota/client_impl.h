#pragma once
#include <memory>
#include <string>

#include "envoy/config/core/v3/grpc_service.pb.h"
#include "envoy/grpc/async_client_manager.h"
#include "envoy/service/rate_limit_quota/v3/rlqs.pb.h"

#include "source/common/grpc/typed_async_client.h"
#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/rate_limit_quota/client.h"
#include "source/extensions/filters/http/rate_limit_quota/quota_bucket.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitQuota {

using ::envoy::service::rate_limit_quota::v3::BucketId;
using ::envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports;

using BucketQuotaUsage =
    ::envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports::BucketQuotaUsage;
using GrpcAsyncClient =
    Grpc::AsyncClient<envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports,
                      envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse>;

// Grpc bidirectional streaming client which handles the communication with RLS server.
class RateLimitClientImpl : public RateLimitClient,
                            public Grpc::AsyncStreamCallbacks<
                                envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse>,
                            public Logger::Loggable<Logger::Id::rate_limit_quota> {
public:
  RateLimitClientImpl(const envoy::config::core::v3::GrpcService& grpc_service,
                      Server::Configuration::FactoryContext& context)
      : aync_client_(context.clusterManager().grpcAsyncClientManager().getOrCreateRawAsyncClient(
            grpc_service, context.scope(), true)) {
    // TODO(tyxia) startStream function is opened on the first request not at time when client is
    // created here.
  }

  void onReceiveMessage(RateLimitQuotaResponsePtr&& response) override;

  // RawAsyncStreamCallbacks
  void onCreateInitialMetadata(Http::RequestHeaderMap&) override {}
  void onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) override {}
  void onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) override {}
  void onRemoteClose(Grpc::Status::GrpcStatus status, const std::string& message) override;

  // RateLimitClient
  void rateLimit(RateLimitQuotaCallbacks& callbacks) override;
  RateLimitQuotaUsageReports buildUsageReport(absl::optional<BucketId> bucket_id);

  // This function get rid of `bucket_usage_`
  RateLimitQuotaUsageReports buildUsageReport2(const BucketId& bucket_id);
  // void sendUsageReport(const BucketId* bucket_id);
  void sendUsageReport(absl::optional<BucketId> bucket_id);

  absl::Status startStream(const StreamInfo::StreamInfo& stream_info);
  void closeStream();
  void send(envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports&& reports,
            bool end_stream);

private:
  // Store the client as the bare object since there is no ownership transfer involved.
  GrpcAsyncClient aync_client_;
  Grpc::AsyncStream<envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports> stream_{};
  RateLimitQuotaCallbacks* callbacks_{};
  // TODO(tyxia) Further look at the use of this flag later.
  bool stream_closed_ = false;

  // TODO(tyxia) Store it outside of filter, as thread local storage!!!!
  struct BucketQuotaUsageInfo {
    BucketQuotaUsage usage;
    std::string domain;
    // The index
    int idx;
  };
  absl::node_hash_map<BucketId, BucketQuotaUsageInfo, BucketIdHash, BucketIdEqual> bucket_usage_;
  // TODO(tyxia) We don't really need to cache the `RateLimitQuotaUsageReports` because we build the
  // report from scrach every time based on `bucket_usage_` above ??
  absl::node_hash_map<std::string, RateLimitQuotaUsageReports> usage_reports_;
};

using RateLimitClientPtr = std::unique_ptr<RateLimitClientImpl>;
using RateLimitClientSharedPtr = std::shared_ptr<RateLimitClientImpl>;
/**
 * Create the rate limit client. It is uniquely owned by each worker thread.
 */
inline RateLimitClientPtr
createRateLimitClient(Server::Configuration::FactoryContext& context,
                      const envoy::config::core::v3::GrpcService& grpc_service) {
  return std::make_unique<RateLimitClientImpl>(grpc_service, context);
}

inline RateLimitClientSharedPtr
createRateLimitGrpcClient(Server::Configuration::FactoryContext& context,
                          const envoy::config::core::v3::GrpcService& grpc_service) {
  return std::make_shared<RateLimitClientImpl>(grpc_service, context);
}

} // namespace RateLimitQuota
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy