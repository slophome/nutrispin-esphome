#pragma once
#include "protocol.h"
#include <algorithm>
#include <vector>

namespace esphome::nutrispin {
struct PairingCandidate {
  uint64_t mac{};
  uint8_t address_type{};
  int rssi{};
  uint32_t seen{};
  wire::Identity identity;
};

// Bounded, per-address assembly: advertising and scan-response reports can
// arrive separately or interleaved from several identical feeders.
class PairingDiscovery {
 public:
  void clear() { entries_.clear(); }
  void observe(wire::Crypto &crypto, uint64_t mac, uint8_t type, int rssi,
               const wire::Bytes &service, const wire::Bytes &manufacturer, uint32_t now) {
    if(!mac || (service.empty() && manufacturer.empty())) return;
    auto it=std::find_if(entries_.begin(),entries_.end(),[mac](const Entry &e){ return e.device.mac==mac; });
    if(it==entries_.end()) {
      if(entries_.size()==12) {
        auto oldest=std::max_element(entries_.begin(),entries_.end(),[now](const Entry &a,const Entry &b){ return uint32_t(now-a.device.seen)<uint32_t(now-b.device.seen); });
        if(uint32_t(now-oldest->device.seen)<10000) return;
        entries_.erase(oldest);
      }
      entries_.push_back({}); it=entries_.end()-1; it->device.mac=mac;
    }
    it->device.address_type=type; it->device.rssi=rssi; it->device.seen=now;
    if(!service.empty()) it->service=service;
    if(!manufacturer.empty()) it->manufacturer=manufacturer;
    it->valid=wire::identity(crypto,it->service,it->manufacturer,it->device.identity)
              && !it->device.identity.bound && it->device.identity.product=="66d1tpmo";
  }
  std::vector<PairingCandidate> available(uint32_t now) const {
    std::vector<PairingCandidate> out;
    for(const auto &entry:entries_)
      if(entry.valid && uint32_t(now-entry.device.seen)<=10000) out.push_back(entry.device);
    return out;
  }
 private:
  struct Entry { PairingCandidate device; wire::Bytes service,manufacturer; bool valid{}; };
  std::vector<Entry> entries_;
};
}  // namespace esphome::nutrispin
