#pragma once
#include <cstdint>

namespace eosio::resource_feed {

struct account_record {
   uint64_t owner;
   int64_t  net_weight;
   int64_t  cpu_weight;
   int64_t  ram_bytes;
   uint32_t net_last_ordinal;
   uint64_t net_value_ex;
   uint64_t net_consumed;
   uint32_t cpu_last_ordinal;
   uint64_t cpu_value_ex;
   uint64_t cpu_consumed;
   uint64_t ram_usage;
};

struct globals_record {
   uint64_t total_net_weight;
   uint64_t total_cpu_weight;
   uint64_t total_ram_bytes;
   uint64_t virtual_net_limit;
   uint64_t virtual_cpu_limit;
};

struct config_record {
   uint32_t cpu_window;
   uint32_t net_window;
};

constexpr uint32_t FEED_MAGIC   = 0x52534644;
constexpr uint16_t FEED_VERSION = 1;

enum class frame_type : uint8_t {
   hello             = 1,
   snapshot_begin    = 2,
   accounts          = 3,
   accounts_removed  = 4,
   globals           = 5,
   config            = 6,
   snapshot_end      = 7,
   block             = 8,
};

} // namespace eosio::resource_feed
