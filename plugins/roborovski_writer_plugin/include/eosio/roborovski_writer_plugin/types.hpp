#pragma once
#include <eosio/chain/block.hpp>
#include <eosio/chain/trace.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace eosio::roborovski_writer {

class writer_exception : public std::runtime_error {
public:
   using std::runtime_error::runtime_error;
};

class io_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

class compression_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

class data_corruption_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

class layout_exception : public writer_exception {
public:
   using writer_exception::writer_exception;
};

using chain::block_id_type;
using chain::packed_transaction_ptr;
using chain::signed_block;
using chain::signed_block_ptr;
using chain::transaction_id_type;
using chain::transaction_trace_ptr;

constexpr uint32_t BLOCKS_PER_SLICE = 10000;
constexpr int      ZSTD_LEVEL       = 3;

constexpr char    SLICE_MAGIC[7]    = {'H', 'W', 'S', 'L', 'I', 'C', 'E'};
constexpr uint8_t SLICE_VERSION     = 1;
constexpr size_t  SLICE_HEADER_SIZE = 32;
constexpr size_t  INDEX_ENTRY_SIZE  = 32;
constexpr size_t  RECORD_OVERHEAD   = 8;

struct cached_trace {
   transaction_trace_ptr  trace;
   packed_transaction_ptr ptrx;
};

struct auth_triple {
   uint32_t account_index    = 0;
   uint32_t permission_index = 0;
   uint64_t seq              = 0;
};

struct account_delta_entry {
   uint32_t account_index = 0;
   int64_t  delta         = 0;
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

struct abi_record {
   uint32_t    block_num = 0;
   uint64_t    contract  = 0;
   std::string json;
};

struct built_block {
   uint32_t                block_num    = 0;
   block_id_type           id;
   uint64_t                glob_min     = 0;
   uint64_t                glob_max     = 0;
   size_t                  action_count = 0;
   std::vector<char>       blob;
   std::vector<abi_record> abis;
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
   uint32_t blocks_per_slice = BLOCKS_PER_SLICE;
   bool     finalized        = false;
   uint64_t glob_min         = 0;
   uint64_t glob_max         = 0;
};

} // namespace eosio::roborovski_writer
