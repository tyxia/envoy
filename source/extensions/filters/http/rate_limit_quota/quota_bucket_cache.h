#pragma once
#include <algorithm>
#include <memory>

#include "envoy/extensions/filters/http/rate_limit_quota/v3/rate_limit_quota.pb.h"
#include "envoy/extensions/filters/http/rate_limit_quota/v3/rate_limit_quota.pb.validate.h"
#include "envoy/service/rate_limit_quota/v3/rlqs.pb.h"
#include "envoy/service/rate_limit_quota/v3/rlqs.pb.validate.h"

#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/rate_limit_quota/client.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitQuota {

using ::envoy::service::rate_limit_quota::v3::BucketId;
using ::envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports;

using BucketAction = ::envoy::service::rate_limit_quota::v3::RateLimitQuotaResponse::BucketAction;
using BucketQuotaUsage =
    ::envoy::service::rate_limit_quota::v3::RateLimitQuotaUsageReports::BucketQuotaUsage;

// Forward declaration
// class RateLimitClientImpl;

// Customized hash and equal struct for `BucketId` hash key.
struct BucketIdHash {
  size_t operator()(const BucketId& bucket_id) const { return MessageUtil::hash(bucket_id); }
};

struct BucketIdEqual {
  bool operator()(const BucketId& id1, const BucketId& id2) const {
    return Protobuf::util::MessageDifferencer::Equals(id1, id2);
  }
};

// Single bucket entry
struct Bucket {
  // Default constructor
  Bucket() = default;
  // Disable copy constructor and assignment.
  Bucket(const Bucket& bucket) = delete;
  Bucket& operator=(const Bucket& bucket) = delete;
  // Default move constructor and assignment.
  Bucket(Bucket&& bucket) = default;
  Bucket& operator=(Bucket&& bucket) = default;

  ~Bucket() {
    // Close stream
    // TODO(tyxia) filter test failure related to here.
    rate_limit_client->closeStream();
  }

  // TODO(tyxia) Each bucket owns the unique grpc client for sending the quota usage report
  // periodically.
  // std::unique_ptr<RateLimitClientImpl> rate_limit_client;
  // TODO(tyxia) Store the abstract interface to avoid cyclic dependency between quota bucket and
  // client.
  std::unique_ptr<RateLimitClient> rate_limit_client;
  // The timer for sending the reports periodically.
  Event::TimerPtr send_reports_timer;
  // Cached bucket action from the response that was received from the RLQS server.
  // BucketAction bucket_action;
  // TODO(tyxia) Thread local storage should take the ownership of all the objects so that
  // it is also responsible for destruction.
  std::unique_ptr<BucketAction> bucket_action;
  // TODO(tyxia) No need to store bucket ID  as it is already the key of BucketsContainer.
  // TODO(tyxia) Seems unused
  BucketQuotaUsage quota_usage;
};

// using BucketsContainer = absl::node_hash_map<BucketId, Bucket, BucketIdHash, BucketIdEqual>;
using BucketsContainer =
    absl::node_hash_map<BucketId, std::unique_ptr<Bucket>, BucketIdHash, BucketIdEqual>;

class ThreadLocalBucket : public Envoy::ThreadLocal::ThreadLocalObject {
public:
  // TODO(tyxia) Here is similar to defer initialization methodology.
  // We have the empty hash map in thread local storage at the beginning and build the map later
  // in the `rate_limit_quota_filter` when the request comes.
  ThreadLocalBucket() = default;

  // Return the buckets. Buckets are returned by reference so that caller site can modify its
  // content.
  BucketsContainer& quotaBuckets() { return quota_buckets_; }
  // Return the quota usage reports.
  RateLimitQuotaUsageReports& quotaUsageReports() { return quota_usage_reports_; }

private:
  // Thread local storage for bucket container and quota usage report.
  BucketsContainer quota_buckets_;
  RateLimitQuotaUsageReports quota_usage_reports_;
};

struct BucketCache {
  BucketCache(Envoy::Server::Configuration::FactoryContext& context) : tls(context.threadLocal()) {
    tls.set([](Envoy::Event::Dispatcher&) { return std::make_shared<ThreadLocalBucket>(); });
  }
  Envoy::ThreadLocal::TypedSlot<ThreadLocalBucket> tls;
};

} // namespace RateLimitQuota
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
