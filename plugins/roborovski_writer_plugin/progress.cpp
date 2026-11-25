#include <eosio/roborovski_writer_plugin/progress.hpp>

#include <eosio/chain/block.hpp>
#include <eosio/chain/block_log.hpp>

#include <cinttypes>
#include <cstdio>

namespace eosio::roborovski_writer {

namespace {

std::string format_bytes(uint64_t n) {
   char buf[32];
   if (n >= 1024ull * 1024 * 1024)
      std::snprintf(buf, sizeof(buf), "%.1f GB", n / (1024.0 * 1024 * 1024));
   else if (n >= 1024ull * 1024)
      std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024));
   else if (n >= 1024)
      std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
   else
      std::snprintf(buf, sizeof(buf), "%" PRIu64 " B", n);
   return buf;
}

} // namespace

std::string format_progress(const progress_sample& s) {
   char head[96];
   if (s.target > 0)
      std::snprintf(head, sizeof(head), "Block: %" PRIu32 " / %" PRIu32 " (%.1f%%)", s.committed, s.target,
                    100.0 * s.committed / s.target);
   else
      std::snprintf(head, sizeof(head), "Block: %" PRIu32 " / ?", s.committed);

   const int64_t bps = s.elapsed_s > 0 ? static_cast<int64_t>(s.blocks / s.elapsed_s) : 0;
   double        per_bytes = 0, per_actions = 0, per_trxs = 0;
   if (s.blocks > 0) {
      per_bytes   = static_cast<double>(s.bytes) / s.blocks;
      per_actions = static_cast<double>(s.actions) / s.blocks;
      per_trxs    = static_cast<double>(s.transactions) / s.blocks;
   }

   char tail[256];
   std::snprintf(tail, sizeof(tail),
                 "\t%" PRId64 " BPS | %s (%.0f B/blk) | %.1f acts/blk, %.1f txns/blk | glob: %" PRIu64
                 " | writer: %.0f of %.0f ms",
                 bps, format_bytes(s.bytes).c_str(), per_bytes, per_actions, per_trxs, s.glob, s.writer_s * 1000,
                 s.elapsed_s * 1000);
   return std::string(head) + tail;
}

uint64_t stored_transaction_count(const chain::signed_block& block) {
   uint64_t n = block.block_num() > 1 ? 1 : 0;
   for (const auto& r : block.transactions)
      if (r.status == chain::transaction_receipt_header::executed ||
          r.status == chain::transaction_receipt_header::soft_fail)
         ++n;
   return n;
}

uint32_t block_log_end(const std::filesystem::path& blocks_dir) {
   try {
      const auto index = blocks_dir / "blocks.index";
      if (!std::filesystem::exists(index))
         return 0;
      const uint64_t entries = std::filesystem::file_size(index) / sizeof(uint64_t);
      const uint32_t first   = chain::block_log::extract_first_block_num(blocks_dir);
      if (entries == 0 || first == 0)
         return 0;
      return static_cast<uint32_t>(first + entries - 1);
   } catch (...) {
      return 0;
   }
}

} // namespace eosio::roborovski_writer
