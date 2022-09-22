#include "source/extensions/filters/http/rate_limit_quota/filter.h"

#include <memory>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitQuota {

void RateLimitQuotaFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  callbacks_ = &callbacks;
}

Http::FilterHeadersStatus RateLimitQuotaFilter::decodeHeaders(Http::RequestHeaderMap&, bool) {
  // Start the stream on the first request.
  if (rate_limit_client_->startStream(callbacks_->streamInfo()).ok()) {
    rate_limit_client_->rateLimit(*this);
    return Envoy::Http::FilterHeadersStatus::StopAllIterationAndWatermark;
  }

  return Envoy::Http::FilterHeadersStatus::Continue;
}

void RateLimitQuotaFilter::onDestroy() { rate_limit_client_->closeStream(); }

void RateLimitQuotaFilter::createMatcher() {
  RateLimitOnMactchActionContext context;
  Matcher::MatchTreeFactory<Http::HttpMatchingData, RateLimitOnMactchActionContext> factory(
      context, factory_context_.getServerFactoryContext(), visitor_);
  if (config_->has_bucket_matchers()) {
    matcher_ = factory.create(config_->bucket_matchers())();
  }
}

// Perform the request matching.
absl::StatusOr<BucketId>
RateLimitQuotaFilter::requestMatching(const Http::RequestHeaderMap& headers) {
  // Initialize the data pointer on first use and reuse it for subsequent requests.
  // This avoids creating the data object for every request, which is expensive.
  if (data_ptr_ == nullptr) {
    if (callbacks_ != nullptr) {
      data_ptr_ = std::make_unique<Http::Matching::HttpMatchingDataImpl>(
          callbacks_->streamInfo().downstreamAddressProvider());
    } else {
      return absl::InternalError("Filter callback has not been initialized yet.");
    }
  }

  if (matcher_ == nullptr) {
    return absl::InternalError("Matcher has not been initialized yet.");
  } else {
    data_ptr_->onRequestHeaders(headers);
    // TODO(tyxia) This function should trigger the CEL expression matching.
    // We need to implement the custom_matcher and factory and register so that CEL matching will be
    // triggered with its own match() method.
    auto match = Matcher::evaluateMatch<Http::HttpMatchingData>(*matcher_, *data_ptr_);
    if (match.result_) {
      std::cout << "Matcher succeed!!!" << std::endl;
      const auto result = match.result_();
      ASSERT(result->typeUrl() == RateLimitOnMactchAction::staticTypeUrl());
      ASSERT(dynamic_cast<RateLimitOnMactchAction*>(result.get()));
      const RateLimitOnMactchAction& match_action =
          static_cast<const RateLimitOnMactchAction&>(*result);
      // return static_cast<const RateLimitOnMactchAction&>(*result);
      return match_action.generateBucketId(*data_ptr_, factory_context_, visitor_);
    } else {
      return absl::InternalError(absl::StrCat("Failed to match the request"));
    }
  }
}

BucketId
RateLimitOnMactchAction::generateBucketId(const Http::Matching::HttpMatchingDataImpl& data,
                                          Server::Configuration::FactoryContext& factory_context,
                                          RateLimitQuotaValidationVisitor& visitor) const {
  BucketId bucket_id;
  std::unique_ptr<Matcher::MatchInputFactory<Http::HttpMatchingData>> input_factory_ptr = nullptr;
  for (const auto& id_builder : setting_.bucket_id_builder().bucket_id_builder()) {
    std::string bucket_id_key = id_builder.first;
    auto builder_method = id_builder.second;

    // Generate the bucket id based on builder method type.
    switch (builder_method.value_specifier_case()) {
    // Retrieve the static string value directly from the config.
    case ValueSpecifierCase::kStringValue: {
      // Build the final bucket id directly from config.
      bucket_id.mutable_bucket()->insert({bucket_id_key, builder_method.string_value()});
      break;
    }
    // Retrieve the dynamic value from the `custom_value` typed extension config.
    case ValueSpecifierCase::kCustomValue: {
      // Initialize the pointer to input factory on first use.
      if (input_factory_ptr == nullptr) {
        input_factory_ptr = std::make_unique<Matcher::MatchInputFactory<Http::HttpMatchingData>>(
            factory_context.messageValidationVisitor(), visitor);
      }
      // Create `DataInput` factory callback from the config.
      Matcher::DataInputFactoryCb<Http::HttpMatchingData> data_input_cb =
          input_factory_ptr->createDataInput(builder_method.custom_value());
      auto result = data_input_cb()->get(data);
      // If result has data.
      if (result.data_) {
        if (!result.data_.value().empty()) {
          // Build the bucket id from the matched result.
          bucket_id.mutable_bucket()->insert({bucket_id_key, result.data_.value()});
        } else {
          // ENVOY_LOG(debug, "Empty matched result.");
        }
      } else {
        // ENVOY_LOG(debug, "Failed to retrieve the result from custom value");
      }
      break;
    }
    case ValueSpecifierCase::VALUE_SPECIFIER_NOT_SET: {
      break;
    }
      PANIC_DUE_TO_CORRUPT_ENUM;
    }
  }
  return bucket_id;
}

// Register the action factory.
REGISTER_FACTORY(RateLimitOnMactchActionFactory,
                 Matcher::ActionFactory<RateLimitOnMactchActionContext>);

} // namespace RateLimitQuota
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
