#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace eosio::chain {
struct signed_block;
}

namespace eosio::roborovski_writer {

struct progress_sample {
   uint32_t committed    = 0;
   uint32_t target       = 0;
   double   elapsed_s    = 0;
   double   writer_s     = 0;
   uint64_t blocks       = 0;
   uint64_t actions      = 0;
   uint64_t transactions = 0;
   uint64_t bytes        = 0;
   uint64_t glob         = 0;
};

// Same fields and order as coreindex's trace sync line, with the writer's own time in place of its worker rate.
std::string format_progress(const progress_sample& s);

// Transactions the block builder records: onblock plus every executed or soft failed receipt.
uint64_t stored_transaction_count(const chain::signed_block& block);

// Last block in the block log under blocks_dir from its index, or 0 when that cannot be read.
uint32_t block_log_end(const std::filesystem::path& blocks_dir);

} // namespace eosio::roborovski_writer
