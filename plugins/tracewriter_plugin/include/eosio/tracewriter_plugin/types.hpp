#pragma once
#include <eosio/chain/block.hpp>
#include <eosio/chain/trace.hpp>

#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace eosio::tracewriter {

class tracewriter_exception : public std::runtime_error {
public:
   explicit tracewriter_exception(const char* what_arg)
      : std::runtime_error(what_arg) {}
   explicit tracewriter_exception(const std::string& what_arg)
      : std::runtime_error(what_arg) {}
};

class io_exception : public tracewriter_exception {
public:
   explicit io_exception(const char* what_arg)
      : tracewriter_exception(what_arg) {}
   explicit io_exception(const std::string& what_arg)
      : tracewriter_exception(what_arg) {}
};

class compression_exception : public tracewriter_exception {
public:
   explicit compression_exception(const char* what_arg)
      : tracewriter_exception(what_arg) {}
   explicit compression_exception(const std::string& what_arg)
      : tracewriter_exception(what_arg) {}
};

class data_corruption_exception : public tracewriter_exception {
public:
   explicit data_corruption_exception(const char* what_arg)
      : tracewriter_exception(what_arg) {}
   explicit data_corruption_exception(const std::string& what_arg)
      : tracewriter_exception(what_arg) {}
};

using chain::block_id_type;
using chain::packed_transaction_ptr;
using chain::signed_block_ptr;
using chain::transaction_id_type;
using chain::transaction_trace_ptr;

struct auth_triple {
   uint32_t account_index;
   uint32_t permission_index;
   uint64_t seq;
};

struct account_delta_entry {
   uint32_t account_index;
   int64_t  delta;
};

struct compressed_action_trace {
   uint64_t                         global_sequence     = 0;
   int64_t                          elapsed             = 0;
   uint64_t                         recv_sequence       = 0;
   uint32_t                         action_ordinal      = 0;
   uint32_t                         creator_ao          = 0;
   uint32_t                         closest_ua_ao       = 0;
   uint32_t                         code_sequence       = 0;
   uint32_t                         abi_sequence        = 0;
   uint32_t                         receiver_index      = 0;
   uint32_t                         contract_name_index = 0;
   uint32_t                         action_name_index   = 0;
   uint32_t                         trx_id_index        = 0;
   uint32_t                         data_index          = 0;
   bool                             context_free        = false;
   std::vector<auth_triple>         auths;
   std::vector<account_delta_entry> account_ram_deltas;
};

struct trx_meta {
   uint8_t                        status           = 0;
   uint32_t                       cpu_usage_us     = 0;
   uint32_t                       net_usage_words  = 0;
   uint32_t                       expiration       = 0;
   uint16_t                       ref_block_num    = 0;
   uint32_t                       ref_block_prefix = 0;
   std::vector<std::vector<char>> signatures;
};

struct block_data {
   uint32_t                         block_num  = 0;
   uint32_t                         block_time = 0;
   uint64_t                         min_glob   = std::numeric_limits<uint64_t>::max();
   uint64_t                         max_glob   = 0;
   block_id_type                    producer_block_id;
   std::vector<transaction_id_type> trx_ids;
   std::vector<trx_meta>            trx_metas;
   std::vector<std::vector<char>>   data_table;
   std::vector<uint64_t>            names_table;

   std::unordered_map<uint64_t, uint32_t>    names_index;
   std::unordered_map<std::string, uint32_t> data_index;

   uint32_t insert_or_get_name(uint64_t name) {
      auto it = names_index.find(name);
      if (it != names_index.end())
         return it->second;
      uint32_t idx = static_cast<uint32_t>(names_table.size());
      names_table.push_back(name);
      names_index[name] = idx;
      return idx;
   }

   uint32_t insert_or_get_data(const std::vector<char>& data) {
      std::string key(data.begin(), data.end());
      auto        it = data_index.find(key);
      if (it != data_index.end())
         return it->second;
      uint32_t idx = static_cast<uint32_t>(data_table.size());
      data_table.push_back(data);
      data_index[key] = idx;
      return idx;
   }

   void update_glob_range(uint64_t glob) {
      min_glob = std::min(min_glob, glob);
      max_glob = std::max(max_glob, glob);
   }

   void clear() {
      block_num         = 0;
      block_time        = 0;
      min_glob          = std::numeric_limits<uint64_t>::max();
      max_glob          = 0;
      producer_block_id = block_id_type();
      trx_ids.clear();
      trx_metas.clear();
      data_table.clear();
      names_table.clear();
      names_index.clear();
      data_index.clear();
   }
};

struct cached_trace {
   transaction_trace_ptr  trace;
   packed_transaction_ptr ptrx;
};

constexpr uint32_t SLICE_MAGIC               = 0x48495354;
constexpr uint32_t SLICE_VERSION             = 1;
constexpr uint32_t DEFAULT_BLOCKS_PER_SLICE  = 10000;

struct slice_header {
   uint32_t magic            = SLICE_MAGIC;
   uint32_t version          = SLICE_VERSION;
   uint32_t start_block      = 0;
   uint32_t max_block        = 0;
   uint32_t blocks_per_slice = DEFAULT_BLOCKS_PER_SLICE;
   char     reserved[12]     = {0};
};

struct blocks_index_entry {
   uint32_t block_num       = 0;
   uint64_t offset          = 0;
   uint32_t compressed_size = 0;
   uint64_t glob_min        = 0;
   uint64_t glob_max        = 0;
};

struct slice_info {
   uint32_t slice_num        = 0;
   uint32_t start_block      = 0;
   uint32_t end_block        = 0;
   uint32_t max_block        = 0;
   uint32_t blocks_per_slice = DEFAULT_BLOCKS_PER_SLICE;
   bool     finalized        = false;
   uint64_t glob_min         = 0;
   uint64_t glob_max         = 0;
};

} // namespace eosio::tracewriter
