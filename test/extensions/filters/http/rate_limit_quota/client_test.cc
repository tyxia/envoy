#include "envoy/config/core/v3/grpc_service.pb.h"

#include "source/common/grpc/common.h"
#include "source/common/http/header_map_impl.h"
#include "source/extensions/filters/http/rate_limit_quota/client.h"
#include "source/extensions/filters/http/rate_limit_quota/client_impl.h"

#include "test/extensions/filters/http/rate_limit_quota/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/status_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitQuota {
namespace {

using Server::Configuration::MockFactoryContext;

using testing::Invoke;
using testing::Unused;

class RateLimitStreamTest : public testing::Test {
public:
  void SetUp() override {
    grpc_service_.mutable_envoy_grpc()->set_cluster_name("rate_limit_quota");
    // Set the expected behavior for async_client_manager in mock context.
    // Note, we need to set it through `MockFactoryContext` rather than `MockAsyncClientManager`
    // directly because the rate limit client object below requires context argument as the input.
    EXPECT_CALL(context_.cluster_manager_.async_client_manager_, getOrCreateRawAsyncClient(_, _, _))
        .WillOnce(Invoke(this, &RateLimitStreamTest::mockCreateAsyncClient));

    client_ = createRateLimitClient(context_, grpc_service_, &reports_);
  }

  Grpc::RawAsyncClientSharedPtr mockCreateAsyncClient(Unused, Unused, Unused) {
    auto async_client = std::make_shared<Grpc::MockAsyncClient>();
    EXPECT_CALL(*async_client, startRaw("envoy.service.rate_limit_quota.v3.RateLimitQuotaService",
                                        "StreamRateLimitQuotas", _, _))
        .WillOnce(Invoke(this, &RateLimitStreamTest::mockStartRaw));

    return async_client;
  }

  Grpc::RawAsyncStream* mockStartRaw(Unused, Unused, Grpc::RawAsyncStreamCallbacks& callbacks,
                                     const Http::AsyncClient::StreamOptions&) {
    stream_callbacks_ = &callbacks;
    return &stream_;
  }

  ~RateLimitStreamTest() override = default;

  NiceMock<MockFactoryContext> context_;
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;

  envoy::config::core::v3::GrpcService grpc_service_;
  Grpc::MockAsyncStream stream_;
  Grpc::RawAsyncStreamCallbacks* stream_callbacks_;
  Grpc::Status::GrpcStatus grpc_status_ = Grpc::Status::WellKnownGrpcStatus::Ok;
  RateLimitClientPtr client_;
  MockRateLimitQuotaCallbacks callbacks_;
  RateLimitQuotaUsageReports reports_;

  bool grpc_closed_ = false;
};

TEST_F(RateLimitStreamTest, OpenAndCloseStream) {
  EXPECT_OK(client_->startStream(stream_info_));
  EXPECT_CALL(stream_, closeStream());
  EXPECT_CALL(stream_, resetStream());
  client_->closeStream();
}

constexpr char SingleBukcetId[] = R"EOF(
  bucket:
    "fairshare_group_id":
      "mock_group"
)EOF";

constexpr char MultipleBukcetId[] = R"EOF(
  bucket:
    "fairshare_group_id":
      "mock_group"
    "fairshare_project_id":
      "mock_project"
    "fairshare_user_id":
      "test"
)EOF";

TEST_F(RateLimitStreamTest, BuildUsageReport) {
  ::envoy::service::rate_limit_quota::v3::BucketId bucket_id;
  TestUtility::loadFromYaml(SingleBukcetId, bucket_id);
  std::string domain = "cloud_12345_67890_td_rlqs";

  EXPECT_OK(client_->startStream(stream_info_));
  RateLimitQuotaUsageReports report = client_->buildUsageReport(domain, bucket_id);
  EXPECT_EQ(report.domain(), domain);
  EXPECT_EQ(report.bucket_quota_usages().size(), 1);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_allowed(), 1);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_denied(), 0);
}

TEST_F(RateLimitStreamTest, BuildMultipleReports) {
  ::envoy::service::rate_limit_quota::v3::BucketId bucket_id;
  TestUtility::loadFromYaml(SingleBukcetId, bucket_id);
  std::string domain = "cloud_12345_67890_td_rlqs";

  EXPECT_OK(client_->startStream(stream_info_));
  // Build the usage report with 2 entries with same domain and bucket id.
  RateLimitQuotaUsageReports report = client_->buildUsageReport(domain, bucket_id);
  report = client_->buildUsageReport(domain, bucket_id);
  EXPECT_EQ(report.domain(), domain);
  EXPECT_EQ(report.bucket_quota_usages().size(), 1);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_allowed(), 2);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_denied(), 0);

  ::envoy::service::rate_limit_quota::v3::BucketId bucket_id2;
  TestUtility::loadFromYaml(MultipleBukcetId, bucket_id2);
  // Build the usage report with the entry with different bucket id which will create a new entry in
  // report.
  report = client_->buildUsageReport(domain, bucket_id2);
  EXPECT_EQ(report.bucket_quota_usages().size(), 2);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_allowed(), 2);
  EXPECT_EQ(report.bucket_quota_usages(1).num_requests_allowed(), 1);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_denied(), 0);

  // Update the usage report with old bucket id.
  report = client_->buildUsageReport(domain, bucket_id);
  EXPECT_EQ(report.bucket_quota_usages().size(), 2);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_allowed(), 3);
  EXPECT_EQ(report.bucket_quota_usages(0).num_requests_denied(), 0);
}

TEST_F(RateLimitStreamTest, SendUsageReport) {
  ::envoy::service::rate_limit_quota::v3::BucketId bucket_id;
  TestUtility::loadFromYaml(SingleBukcetId, bucket_id);
  std::string domain = "cloud_12345_67890_td_rlqs";
  EXPECT_OK(client_->startStream(stream_info_));
  bool end_stream = true;
  // Send quota usage report and ensure that we get it.
  EXPECT_CALL(stream_, sendMessageRaw_(_, end_stream));
  client_->sendUsageReport(domain, bucket_id);
  EXPECT_CALL(stream_, closeStream());
  EXPECT_CALL(stream_, resetStream());
  client_->closeStream();
}

// TEST_F(RateLimitStreamTest, SendMultipleUsageReport) {
// }

TEST_F(RateLimitStreamTest, SendRequestAndReceiveResponse) {
  EXPECT_OK(client_->startStream(stream_info_));
  ASSERT_NE(stream_callbacks_, nullptr);

  auto empty_request_headers = Http::RequestHeaderMapImpl::create();
  stream_callbacks_->onCreateInitialMetadata(*empty_request_headers);
  auto empty_response_headers = Http::ResponseHeaderMapImpl::create();
  stream_callbacks_->onReceiveInitialMetadata(std::move(empty_response_headers));

  // Send empty report and ensure that we get it.
  EXPECT_CALL(stream_, sendMessageRaw_(_, true));
  client_->rateLimit(callbacks_);

  // `onQuotaResponse` callback is expected to be called.
  EXPECT_CALL(callbacks_, onQuotaResponse);
  envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse resp;
  auto response_buf = Grpc::Common::serializeMessage(resp);
  EXPECT_TRUE(stream_callbacks_->onReceiveMessageRaw(std::move(response_buf)));

  auto empty_response_trailers = Http::ResponseTrailerMapImpl::create();
  stream_callbacks_->onReceiveTrailingMetadata(std::move(empty_response_trailers));

  EXPECT_CALL(stream_, closeStream());
  EXPECT_CALL(stream_, resetStream());
  client_->closeStream();
  client_->onRemoteClose(0, "");
}

} // namespace
} // namespace RateLimitQuota
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
