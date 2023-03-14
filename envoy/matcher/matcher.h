#pragma once

#include <memory>
#include <string>

#include "envoy/common/pure.h"
#include "envoy/config/common/matcher/v3/matcher.pb.h"
#include "envoy/config/core/v3/extension.pb.h"
#include "envoy/config/typed_config.h"
#include "envoy/protobuf/message_validator.h"

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "xds/type/matcher/v3/matcher.pb.h"

namespace Envoy {

namespace Server {
namespace Configuration {
class ServerFactoryContext;
}
} // namespace Server

namespace Matcher {

// This file describes a MatchTree<DataType>, which traverses a tree of matches until it
// either matches (resulting in either an action or a new tree to traverse) or doesn't match.
// The matching might stop early if either the data is not available at all yet, or if more data
// might result in a match.

// By returning a new tree when an OnMatch results in a new tree, matching can be resumed from
// this tree should more data be required to complete matching. This avoids having to start
// from the beginning every time. At some point we might support resuming for any node in the match
// tree: this requires some careful handling of tracking which on_no_match to use should we fail to
// match.
//
// All the matching is performed on strings: a DataInput<DataType> is used to extract a specific
// string from an instance of DataType, while an InputMatcher is used to determine whether the
// extracted string is a match.
//
// For example, DataType might be the type HttpDataInput, allowing
// for the use of HttpRequestHeaders : DataInput<HttpDataInput>, which is configured with the name
// of the header to extract from the request headers.
//
// In cases where the data to match on becomes available over time, this would be fed into the
// DataType over time, allowing matching to be re-attempted as more data is made available. As such
// whenever we extract data from a DataInput, we make note of whether the data might change and
// pause matching until we either match or have all the data. It would then fall on the caller to
// both provide more information to the DataType and to resume matching.
template <class DataType> class MatchTree;

template <class DataType> using MatchTreeSharedPtr = std::shared_ptr<MatchTree<DataType>>;
template <class DataType> using MatchTreePtr = std::unique_ptr<MatchTree<DataType>>;
template <class DataType> using MatchTreeFactoryCb = std::function<MatchTreePtr<DataType>()>;

/**
 * Action provides the interface for actions to perform when a match occurs. It provides no
 * functions, as implementors are expected to downcast this to a more specific action.
 */
class Action {
public:
  virtual ~Action() = default;

  /**
   * The underlying type of this action. This can be used to determine which
   * type this action is before attempting to cast it.
   */
  virtual absl::string_view typeUrl() const PURE;

  /**
   * Helper to convert this action to its underlying type.
   */
  template <class T> const T& getTyped() const {
    ASSERT(dynamic_cast<const T*>(this) != nullptr);
    return static_cast<const T&>(*this);
  }
};

using ActionPtr = std::unique_ptr<Action>;
using ActionFactoryCb = std::function<ActionPtr()>;

template <class ActionFactoryContext> class ActionFactory : public Config::TypedFactory {
public:
  virtual ActionFactoryCb
  createActionFactoryCb(const Protobuf::Message& config,
                        ActionFactoryContext& action_factory_context,
                        ProtobufMessage::ValidationVisitor& validation_visitor) PURE;

  std::string category() const override { return "envoy.matching.action"; }
};

// On match, we either return the action to perform or another match tree to match against.
template <class DataType> struct OnMatch {
  const ActionFactoryCb action_cb_;
  const MatchTreeSharedPtr<DataType> matcher_;
};
template <class DataType> using OnMatchFactoryCb = std::function<OnMatch<DataType>()>;

template <class DataType> class OnMatchFactory {
public:
  virtual ~OnMatchFactory() = default;

  // Instantiates a nested matcher sub-tree or an action.
  // Returns absl::nullopt if neither sub-tree or action is specified.
  virtual absl::optional<OnMatchFactoryCb<DataType>>
  createOnMatch(const xds::type::matcher::v3::Matcher::OnMatch&) PURE;
  // Instantiates a nested matcher sub-tree or an action.
  // Returns absl::nullopt if neither sub-tree or action is specified.
  virtual absl::optional<OnMatchFactoryCb<DataType>>
  createOnMatch(const envoy::config::common::matcher::v3::Matcher::OnMatch&) PURE;
};

/**
 * State enum for the result of an attempted match.
 */
enum class MatchState {
  /**
   * The match could not be completed, e.g. due to the required data not being available.
   */
  UnableToMatch,
  /**
   * The match was completed.
   */
  MatchComplete,
};

/**
 * MatchTree provides the interface for performing matches against the data provided by DataType.
 */
template <class DataType> class MatchTree {
public:
  virtual ~MatchTree() = default;

  // The result of a match. There are three possible results:
  // - The match could not be completed (match_state_ == MatchState::UnableToMatch)
  // - The match was completed, no match found (match_state_ == MatchState::MatchComplete, on_match_
  // = {})
  // - The match was complete, match found (match_state_ == MatchState::MatchComplete, on_match_ =
  // something).
  struct MatchResult {
    const MatchState match_state_;
    const absl::optional<OnMatch<DataType>> on_match_;
  };

  // Attempts to match against the matching data (which should contain all the data requested via
  // matching requirements). If the match couldn't be completed, {false, {}} will be returned.
  // If a match result was determined, {true, action} will be returned. If a match result was
  // determined to be no match, {true, {}} will be returned.
  virtual MatchResult match(const DataType& matching_data) PURE;
};

template <class DataType> using MatchTreeSharedPtr = std::shared_ptr<MatchTree<DataType>>;

// InputMatcher provides the interface for determining whether an input value matches.
class InputMatcher {
public:
  virtual ~InputMatcher() = default;

  /**
   * Whether the provided input is a match.
   * @param absl::optional<absl::string_view> the value to match on. Will be absl::nullopt if the
   * lookup failed.
   */
  virtual bool match(absl::optional<absl::string_view> input) PURE;
};

using InputMatcherPtr = std::unique_ptr<InputMatcher>;
using InputMatcherFactoryCb = std::function<InputMatcherPtr()>;

/**
 * Factory for registering custom input matchers.
 */
class InputMatcherFactory : public Config::TypedFactory {
public:
  virtual InputMatcherFactoryCb
  createInputMatcherFactoryCb(const Protobuf::Message& config,
                              Server::Configuration::ServerFactoryContext& factory_context) PURE;

  std::string category() const override { return "envoy.matching.input_matchers"; }
};

enum class DataAvailability {
  // The data is not yet available.
  NotAvailable,
  // Some data is available, but more might arrive.
  MoreDataMightBeAvailable,
  // All the data is available.
  AllDataAvailable
};

// The result of retrieving data from a DataInput. As the data is generally made available
// over time (e.g. as more of the stream reaches the proxy), data might become increasingly
// available. This return type allows the DataInput to indicate this, as this might influence
// the match decision.
//
// Conceptually the data availability should start at being NotAvailable, transition to
// MoreDataMightBeAvailable (optional, this doesn't make sense for all data) and finally
// AllDataAvailable as the data becomes available.
template <class ResultDataType = std::string> struct DataInputGetResult {
  DataAvailability data_availability_;
  // The resulting data. This will be absl::nullopt if we don't have sufficient data available (as
  // per data_availability_) or because no value was extracted. For example, consider a DataInput
  // which attempts to look a key up in the map: if we don't have access to the map yet, we return
  // absl::nullopt with NotAvailable. If we have the entire map, but the key doesn't exist in the
  // map, we return absl::nullopt with AllDataAvailable.
  absl::optional<ResultDataType> data_;

  // For pretty printing.
  friend std::ostream& operator<<(std::ostream& out, const DataInputGetResult& result) {
    if (result.data_.has_value()) {
      out << "data input: " << result.data_.value();
    } else {
      out << "n/a";
    }
    // out << "data input: " << (result.data_ ? result.data_.value() : "n/a");
    switch (result.data_availability_) {
    case DataAvailability::NotAvailable:
      out << " (not available)";
      break;
    case DataAvailability::MoreDataMightBeAvailable:
      out << " (more data available)";
      break;
    case DataAvailability::AllDataAvailable:;
    }
    return out;
  }
};

// Template deduction guide for DataInputGetResult.
template <class ResultDataType>
DataInputGetResult(DataAvailability data_availability_, ResultDataType data_)
    -> DataInputGetResult<std::string>;

/**
 * Interface for types providing a way to extract a string from the DataType to perform matching
 * on.
 */
template <class DataType, class ResultDataType = std::string> class DataInput {
public:
  virtual ~DataInput() = default;

  virtual DataInputGetResult<ResultDataType> get(const DataType& data) const PURE;
};

template <class DataType, class ResultDataType = std::string>
using DataInputPtr = std::unique_ptr<DataInput<DataType, ResultDataType>>;
template <class DataType, class ResultDataType = std::string>
using DataInputFactoryCb = std::function<DataInputPtr<DataType, ResultDataType>()>;

/**
 * Factory for data inputs.
 */
template <class DataType, class ResultDataType = std::string>
class DataInputFactory : public Config::TypedFactory {
public:
  /**
   * Creates a DataInput from the provided config.
   */
  virtual DataInputFactoryCb<DataType, ResultDataType>
  createDataInputFactoryCb(const Protobuf::Message& config,
                           ProtobufMessage::ValidationVisitor& validation_visitor) PURE;

  /**
   * The category of this factory depends on the DataType, so we require a name() function to exist
   * that allows us to get a string representation of the data type for categorization.
   */
  std::string category() const override {
    // Static assert to guide implementors to understand what is required.
    static_assert(std::is_convertible<absl::string_view, decltype(DataType::name())>(),
                  "DataType must implement valid name() function");
    return fmt::format("envoy.matching.{}.input", DataType::name());
  }
};

/**
 * Interface for types providing a way to use a string for matching without depending on protocol
 * data. As a result, these can be used for all protocols.
 */
// TODO(tyxia) Maybe no need to change here
template <class ResultDataType = std::string> class CommonProtocolInput {
public:
  virtual ~CommonProtocolInput() = default;
  virtual absl::optional<ResultDataType> get() PURE;
};
// using CommonProtocolInputPtr = std::unique_ptr<CommonProtocolInput>;
// using CommonProtocolInputFactoryCb = std::function<CommonProtocolInputPtr()>;

template <class ResultDataType = std::string>
using CommonProtocolInputPtr = std::unique_ptr<CommonProtocolInput<ResultDataType>>;
template <class ResultDataType = std::string>
using CommonProtocolInputFactoryCb = std::function<CommonProtocolInputPtr<ResultDataType>()>;

/**
 * Factory for CommonProtocolInput.
 */
template <class ResultDataType = std::string>
class CommonProtocolInputFactory : public Config::TypedFactory {
public:
  /**
   * Creates a CommonProtocolInput from the provided config.
   */
  virtual CommonProtocolInputFactoryCb<ResultDataType>
  createCommonProtocolInputFactoryCb(const Protobuf::Message& config,
                                     ProtobufMessage::ValidationVisitor& validation_visitor) PURE;

  std::string category() const override { return "envoy.matching.common_inputs"; }
};

/**
 * Factory for registering custom matchers.
 */
template <class DataType> class CustomMatcherFactory : public Config::TypedFactory {
public:
  virtual MatchTreeFactoryCb<DataType>
  createCustomMatcherFactoryCb(const Protobuf::Message& config,
                               Server::Configuration::ServerFactoryContext& factory_context,
                               DataInputFactoryCb<DataType> data_input,
                               absl::optional<OnMatchFactoryCb<DataType>> on_no_match,
                               OnMatchFactory<DataType>& on_match_factory) PURE;
  std::string category() const override {
    // Static assert to guide implementors to understand what is required.
    static_assert(std::is_convertible<absl::string_view, decltype(DataType::name())>(),
                  "DataType must implement valid name() function");
    return fmt::format("envoy.matching.{}.custom_matchers", DataType::name());
  }
};

} // namespace Matcher
} // namespace Envoy

// NOLINT(namespace-envoy)
namespace fmt {
// Allow fmtlib to use operator << defined in DataInputGetResult
// TODO(tyxia) Different from g3
template <> struct formatter<::Envoy::Matcher::DataInputGetResult<>> : ostream_formatter {};
} // namespace fmt
