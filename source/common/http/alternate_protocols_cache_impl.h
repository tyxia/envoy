#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "envoy/common/key_value_store.h"
#include "envoy/common/optref.h"
#include "envoy/common/time.h"
#include "envoy/http/alternate_protocols_cache.h"

#include "source/common/common/logger.h"

#include "absl/strings/string_view.h"
#include "quiche/common/quiche_linked_hash_map.h"

namespace Envoy {
namespace Http {

// An implementation of AlternateProtocolsCache.
// See: source/docs/http3_upstream.md
//
// The primary purpose of this cache is to cache alternate protocols entries.
// Secondarily, it maps origins to srtt information, useful for
// tuning 0-rtt timeouts if the alternate protocol is HTTP/3.
class AlternateProtocolsCacheImpl : public AlternateProtocolsCache,
                                    Logger::Loggable<Logger::Id::alternate_protocols_cache> {
public:
  AlternateProtocolsCacheImpl(TimeSource& time_source, std::unique_ptr<KeyValueStore>&& store,
                              size_t max_entries);
  ~AlternateProtocolsCacheImpl() override;

  // Captures the data tracked per origin; the alternate protocols supported,
  // and last smoothed round trip time, if available.
  struct OriginData {
    std::vector<AlternateProtocol> protocols;
    std::chrono::microseconds srtt;
  };

  // Converts an Origin to a string which can be parsed by stringToOrigin.
  static std::string originToString(const AlternateProtocolsCache::Origin& origin);
  // Converts a string from originToString back to structured format.
  static absl::optional<AlternateProtocolsCache::Origin> stringToOrigin(const std::string& str);

  // Convert origin data to a string to cache to the key value
  // store. Note that in order to determine the lifetime of entries, this
  // function will serialize ma= as absolute time from the epoch rather than
  // relative time.
  // This function also does not do standards-required normalization. Entries requiring
  // normalization will simply not be read from cache.
  // The string format is:
  // protocols|rtt
  static std::string originDataToStringForCache(const std::vector<AlternateProtocol>& protocols,
                                                std::chrono::microseconds srtt);
  // Parse an origin data into structured data, or absl::nullopt
  // if it is empty or invalid.
  // If from_cache is true, it is assumed the string was serialized using
  // protocolsToStringForCache and the the ma fields will be parsed as absolute times
  // rather than relative time.
  static absl::optional<OriginData> originDataFromString(absl::string_view origin_data,
                                                         TimeSource& time_source,
                                                         bool from_cache = false);

  // AlternateProtocolsCache
  void setAlternatives(const Origin& origin, std::vector<AlternateProtocol>& protocols) override;
  void setSrtt(const Origin& origin, std::chrono::microseconds srtt) override;
  std::chrono::microseconds getSrtt(const Origin& origin) const override;
  OptRef<const std::vector<AlternateProtocol>> findAlternatives(const Origin& origin) override;
  size_t size() const override;

private:
  void setAlternativesImpl(const Origin& origin, std::vector<AlternateProtocol>& protocols);
  // Time source used to check expiration of entries.
  TimeSource& time_source_;

  struct OriginHash {
    size_t operator()(const Origin& origin) const {
      // Multiply the hashes by the magic number 37 to spread the bits around.
      size_t hash = std::hash<std::string>()(origin.scheme_) +
                    37 * (std::hash<std::string>()(origin.hostname_) +
                          37 * std::hash<uint32_t>()(origin.port_));
      return hash;
    }
  };

  // Map from origin to list of alternate protocols.
  quiche::QuicheLinkedHashMap<Origin, OriginData, OriginHash> protocols_;

  // The key value store, if flushing to persistent storage.
  std::unique_ptr<KeyValueStore> key_value_store_;

  const size_t max_entries_;
};

} // namespace Http
} // namespace Envoy
