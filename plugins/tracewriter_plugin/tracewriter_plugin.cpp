#include <eosio/tracewriter_plugin/tracewriter_plugin.hpp>
#include <eosio/tracewriter_plugin/block_serializer.hpp>
#include <eosio/tracewriter_plugin/slice_writer.hpp>
#include <eosio/tracewriter_plugin/types.hpp>

#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/resource_monitor_plugin/resource_monitor_plugin.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <sstream>

namespace eosio {
using namespace chain;
using namespace tracewriter;
using boost::signals2::scoped_connection;

static auto _tracewriter_plugin = application::register_plugin<tracewriter_plugin>();

const std::string logger_name("tracewriter");
fc::logger        _tracewriter_log;

const std::string logger_perf_name("tracewriter_perf");
fc::logger        _tracewriter_perf_log;

const std::string logger_io_name("tracewriter_io");
fc::logger        _tracewriter_io_log;

constexpr uint32_t PERF_LOG_INTERVAL = 10000;

struct tracewriter_plugin_impl {
private:
   chain_plugin*         chain_plug = nullptr;
   std::filesystem::path output_dir;
   uint32_t              blocks_per_slice = DEFAULT_BLOCKS_PER_SLICE;
   bool                  log_perf         = false;

   std::optional<scoped_connection> applied_transaction_connection;
   std::optional<scoped_connection> block_start_connection;
   std::optional<scoped_connection> accepted_block_connection;
   std::optional<scoped_connection> irreversible_block_connection;

   std::map<uint32_t, std::map<transaction_id_type, cached_trace>> pending_blocks;
   std::map<uint32_t, std::optional<cached_trace>>                 pending_onblocks;
   std::map<uint32_t, signed_block_ptr>                            pending_block_ptrs;

   std::map<transaction_id_type, cached_trace> cached_traces;
   std::optional<cached_trace>                 onblock_trace;

   block_data                           current_block;
   std::vector<compressed_action_trace> current_cats;
   std::vector<uint32_t>                current_cat_offsets;
   block_serializer                     serializer;
   std::unique_ptr<slice_writer>        writer;

   struct perf_stats {
      uint64_t blocks_processed  = 0;
      uint64_t actions_processed = 0;
      uint64_t bytes_written     = 0;

      void reset() { blocks_processed = actions_processed = bytes_written = 0; }
   };
   perf_stats                            stats;
   std::chrono::steady_clock::time_point stats_start_time;

public:
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

   void on_applied_transaction(const transaction_trace_ptr& trace, const packed_transaction_ptr& ptrx);
   void on_block_start(uint32_t block_num);
   void on_accepted_block(const signed_block_ptr& block, const block_id_type& id);
   void on_irreversible_block(const signed_block_ptr& block, const block_id_type& id);

private:
   void clear_caches();
   void clear_pending_blocks_from(uint32_t block_num);
   void process_irreversible_block(uint32_t block_num);
   void log_perf_stats(uint32_t block_num);

   compressed_action_trace compress_action_trace(const action_trace& at, uint32_t trx_id_index);
   void                    process_transaction_trace(const cached_trace& ct, uint32_t trx_id_index);
};

void tracewriter_plugin_impl::plugin_initialize(const variables_map& options) {
   try {
      chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT(chain_plug, chain::missing_chain_plugin_exception, "chain_plugin required");
      auto& chain = chain_plug->chain();

      auto dir_option = options.at("tracewriter-dir").as<std::filesystem::path>();
      if (dir_option.is_relative())
         output_dir = app().data_dir() / dir_option;
      else
         output_dir = dir_option;

      if (options.count("tracewriter-blocks-per-slice"))
         blocks_per_slice = options.at("tracewriter-blocks-per-slice").as<uint32_t>();

      if (options.count("tracewriter-log-perf"))
         log_perf = options.at("tracewriter-log-perf").as<bool>();

      std::filesystem::create_directories(output_dir);

      if (auto resmon = app().find_plugin<resource_monitor_plugin>())
         resmon->monitor_directory(output_dir);

      fc_ilog(_tracewriter_log, "Tracewriter output directory: ${d}", ("d", output_dir.string()));
      fc_ilog(_tracewriter_log, "Blocks per slice: ${n}", ("n", blocks_per_slice));

      writer = std::make_unique<slice_writer>(output_dir, blocks_per_slice, _tracewriter_log);

      uint32_t last_block = writer->get_last_written_block();
      fc_ilog(_tracewriter_log, "Resuming from block: ${b}", ("b", last_block));

      stats_start_time = std::chrono::steady_clock::now();

      applied_transaction_connection.emplace(chain.applied_transaction().connect(
          [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
             on_applied_transaction(std::get<0>(t), std::get<1>(t));
          }));
      accepted_block_connection.emplace(chain.accepted_block().connect([&](const block_signal_params& t) {
         const auto& [block, id] = t;
         on_accepted_block(block, id);
      }));
      irreversible_block_connection.emplace(chain.irreversible_block().connect([&](const block_signal_params& t) {
         const auto& [block, id] = t;
         on_irreversible_block(block, id);
      }));
      block_start_connection.emplace(
          chain.block_start().connect([&](uint32_t block_num) { on_block_start(block_num); }));
   }
   FC_LOG_AND_RETHROW()
}

void tracewriter_plugin_impl::plugin_startup() {
   fc_ilog(_tracewriter_log, "Tracewriter plugin started");
   stats_start_time = std::chrono::steady_clock::now();
}

void tracewriter_plugin_impl::plugin_shutdown() {
   fc_ilog(_tracewriter_log, "Tracewriter plugin shutting down...");
   applied_transaction_connection.reset();
   accepted_block_connection.reset();
   irreversible_block_connection.reset();
   block_start_connection.reset();
   if (writer) {
      writer->save_metadata();
      fc_ilog(_tracewriter_log, "Wrote ${n} slices, last block: ${b}",
              ("n", writer->get_slice_infos().size())("b", writer->get_last_written_block()));
   }
   fc_ilog(_tracewriter_log, "Tracewriter plugin shutdown complete");
}

void tracewriter_plugin_impl::on_applied_transaction(const transaction_trace_ptr&  trace,
                                                     const packed_transaction_ptr& ptrx) {
   if (!trace->receipt)
      return;
   if (trace->receipt->status != transaction_receipt_header::executed &&
       trace->receipt->status != transaction_receipt_header::soft_fail) {
      return;
   }
   if (is_onblock(*trace)) {
      onblock_trace.emplace(cached_trace{trace, ptrx});
   } else if (trace->failed_dtrx_trace) {
      cached_traces[trace->failed_dtrx_trace->id] = {trace, ptrx};
   } else {
      cached_traces[trace->id] = {trace, ptrx};
   }
}

void tracewriter_plugin_impl::on_block_start(uint32_t block_num) {
   clear_caches();
   if (pending_blocks.count(block_num)) {
      clear_pending_blocks_from(block_num);
   }
}

void tracewriter_plugin_impl::clear_caches() {
   cached_traces.clear();
   onblock_trace.reset();
   current_block.clear();
   current_cats.clear();
   current_cat_offsets.clear();
}

compressed_action_trace tracewriter_plugin_impl::compress_action_trace(const action_trace& at, uint32_t trx_id_index) {
   compressed_action_trace cat;

   cat.action_ordinal = at.action_ordinal.value;
   cat.creator_ao     = at.creator_action_ordinal.value;
   cat.closest_ua_ao  = at.closest_unnotified_ancestor_action_ordinal.value;
   cat.context_free   = at.context_free;
   cat.elapsed        = at.elapsed.count();
   cat.trx_id_index   = trx_id_index;

   if (at.receipt) {
      cat.global_sequence = at.receipt->global_sequence;
      cat.recv_sequence   = at.receipt->recv_sequence;
      cat.code_sequence   = at.receipt->code_sequence.value;
      cat.abi_sequence    = at.receipt->abi_sequence.value;

      current_block.update_glob_range(cat.global_sequence);
   }

   cat.receiver_index      = current_block.insert_or_get_name(at.receiver.to_uint64_t());
   cat.contract_name_index = current_block.insert_or_get_name(at.act.account.to_uint64_t());
   cat.action_name_index   = current_block.insert_or_get_name(at.act.name.to_uint64_t());

   std::vector<char> action_data(at.act.data.begin(), at.act.data.end());
   cat.data_index = current_block.insert_or_get_data(action_data);

   for (size_t i = 0; i < at.act.authorization.size(); ++i) {
      const auto& auth = at.act.authorization[i];
      auth_triple at_auth;
      at_auth.account_index    = current_block.insert_or_get_name(auth.actor.to_uint64_t());
      at_auth.permission_index = current_block.insert_or_get_name(auth.permission.to_uint64_t());
      if (at.receipt && i < at.receipt->auth_sequence.size()) {
         auto it     = at.receipt->auth_sequence.find(auth.actor);
         at_auth.seq = (it != at.receipt->auth_sequence.end()) ? it->second : 0;
      } else {
         at_auth.seq = 0;
      }
      cat.auths.push_back(at_auth);
   }

   for (const auto& delta : at.account_ram_deltas) {
      account_delta_entry ade;
      ade.account_index = current_block.insert_or_get_name(delta.account.to_uint64_t());
      ade.delta         = delta.delta;
      cat.account_ram_deltas.push_back(ade);
   }

   return cat;
}

void tracewriter_plugin_impl::process_transaction_trace(const cached_trace& ct, uint32_t trx_id_index) {
   const auto& trace = ct.trace;
   const auto& ptrx  = ct.ptrx;

   trx_meta meta;
   if (trace->receipt) {
      meta.status          = static_cast<uint8_t>(trace->receipt->status);
      meta.cpu_usage_us    = trace->receipt->cpu_usage_us;
      meta.net_usage_words = trace->receipt->net_usage_words.value;
   }

   const auto& trx_header = ptrx->get_transaction();
   meta.expiration        = trx_header.expiration.sec_since_epoch();
   meta.ref_block_num     = trx_header.ref_block_num;
   meta.ref_block_prefix  = trx_header.ref_block_prefix;

   for (const auto& sig : ptrx->get_signatures()) {
      auto packed = fc::raw::pack(sig);
      meta.signatures.push_back(std::move(packed));
   }
   current_block.trx_metas.push_back(std::move(meta));

   uint32_t cat_offset = static_cast<uint32_t>(current_cats.size());
   for (const auto& at : trace->action_traces) {
      if (!at.context_free) {
         current_cats.push_back(compress_action_trace(at, trx_id_index));
      }
   }
   current_cat_offsets.push_back(cat_offset);
}

void tracewriter_plugin_impl::on_accepted_block(const signed_block_ptr& block, const block_id_type& id) {
   try {
      const uint32_t block_num = block->block_num();

      pending_blocks[block_num]     = std::move(cached_traces);
      pending_onblocks[block_num]   = std::move(onblock_trace);
      pending_block_ptrs[block_num] = block;

      clear_caches();
   } catch (const tracewriter_exception& e) {
      fc_elog(_tracewriter_log, "tracewriter_exception: ${what}", ("what", e.what()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from: ${e}",
                ("e", e.what()));
   } catch (const fc::exception& e) {
      fc_elog(_tracewriter_log, "fc::exception: ${details}", ("details", e.to_detail_string()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from");
   } catch (const std::exception& e) {
      fc_elog(_tracewriter_log, "std::exception: ${what}", ("what", e.what()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from: ${e}",
                ("e", e.what()));
   }
}

void tracewriter_plugin_impl::on_irreversible_block(const signed_block_ptr& block, const block_id_type& id) {
   try {
      const uint32_t block_num = block->block_num();

      process_irreversible_block(block_num);

      if (log_perf && stats.blocks_processed > 0 && stats.blocks_processed % PERF_LOG_INTERVAL == 0) {
         log_perf_stats(block_num);
      }
   } catch (const tracewriter_exception& e) {
      fc_elog(_tracewriter_log, "tracewriter_exception in on_irreversible_block: ${what}", ("what", e.what()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from: ${e}",
                ("e", e.what()));
   } catch (const fc::exception& e) {
      fc_elog(_tracewriter_log, "fc::exception in on_irreversible_block: ${details}",
              ("details", e.to_detail_string()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from");
   } catch (const std::exception& e) {
      fc_elog(_tracewriter_log, "std::exception in on_irreversible_block: ${what}", ("what", e.what()));
      appbase::app().quit();
      EOS_THROW(chain::plugin_exception, "Tracewriter encountered an error which it cannot recover from: ${e}",
                ("e", e.what()));
   }
}

void tracewriter_plugin_impl::process_irreversible_block(uint32_t block_num) {
   auto traces_it  = pending_blocks.find(block_num);
   auto onblock_it = pending_onblocks.find(block_num);
   auto block_it   = pending_block_ptrs.find(block_num);

   if (traces_it == pending_blocks.end() || block_it == pending_block_ptrs.end()) {
      return;
   }

   const auto& block   = block_it->second;
   const auto& traces  = traces_it->second;
   const auto& onblock = (onblock_it != pending_onblocks.end()) ? onblock_it->second : std::nullopt;

   current_block.clear();
   current_cats.clear();
   current_cat_offsets.clear();

   current_block.block_num         = block_num;
   current_block.block_time        = block->timestamp.to_time_point().sec_since_epoch();
   current_block.producer_block_id = block->calculate_id();

   if (onblock) {
      transaction_id_type onblock_id;
      current_block.trx_ids.push_back(onblock_id);
      process_transaction_trace(*onblock, 0);
   }

   for (const auto& r : block->transactions) {
      transaction_id_type trx_id;
      if (std::holds_alternative<transaction_id_type>(r.trx)) {
         trx_id = std::get<transaction_id_type>(r.trx);
      } else {
         trx_id = std::get<packed_transaction>(r.trx).id();
      }

      uint32_t trx_id_index = static_cast<uint32_t>(current_block.trx_ids.size());
      current_block.trx_ids.push_back(trx_id);

      auto it = traces.find(trx_id);
      if (it != traces.end()) {
         process_transaction_trace(it->second, trx_id_index);
      }
   }

   serializer.clear();
   serializer.write_block_blob(current_block, current_cats, current_cat_offsets);

   uint64_t min_glob = current_block.min_glob;
   uint64_t max_glob = current_block.max_glob;
   if (min_glob == std::numeric_limits<uint64_t>::max()) {
      min_glob = 0;
   }

   writer->write_block(block_num, min_glob, max_glob, serializer.data());

   stats.blocks_processed++;
   stats.actions_processed += current_cats.size();
   stats.bytes_written += serializer.data().size();

   pending_blocks.erase(traces_it);
   pending_onblocks.erase(block_num);
   pending_block_ptrs.erase(block_it);
}

void tracewriter_plugin_impl::clear_pending_blocks_from(uint32_t block_num) {
   auto it = pending_blocks.lower_bound(block_num);
   while (it != pending_blocks.end()) {
      pending_onblocks.erase(it->first);
      pending_block_ptrs.erase(it->first);
      it = pending_blocks.erase(it);
   }
}

void tracewriter_plugin_impl::log_perf_stats(uint32_t block_num) {
   auto now     = std::chrono::steady_clock::now();
   auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_start_time).count();

   uint64_t bytes_compressed = writer->get_bytes_compressed();

   double compression_ratio =
       stats.bytes_written > 0 ? static_cast<double>(bytes_compressed) / stats.bytes_written * 100.0 : 0.0;

   double blocks_per_sec = wall_ms > 0 ? static_cast<double>(stats.blocks_processed) * 1000.0 / wall_ms : 0;

   uint32_t mb_per_sec_x10 =
       wall_ms > 0 ? static_cast<uint32_t>(stats.bytes_written * 10000 / 1024 / 1024 / wall_ms) : 0;

   std::ostringstream mbs_str;
   mbs_str << (mb_per_sec_x10 / 10) << "." << (mb_per_sec_x10 % 10);

   fc_ilog(_tracewriter_perf_log,
           "PERF block ${b}: ${blks} blks in ${wall}ms (${bps} blk/s, ${mbs} MB/s) | "
           "actions=${acts} bytes=${bytes} compressed=${ratio}%",
           ("b", block_num)("blks", stats.blocks_processed)("wall", wall_ms)("bps", static_cast<uint32_t>(blocks_per_sec))(
               "mbs", mbs_str.str())("acts", stats.actions_processed)("bytes", stats.bytes_written)(
               "ratio", static_cast<uint32_t>(compression_ratio)));

   stats.reset();
   stats_start_time = now;
}

tracewriter_plugin::tracewriter_plugin()
   : my(new tracewriter_plugin_impl()) {}

tracewriter_plugin::~tracewriter_plugin() = default;

void tracewriter_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto options = cfg.add_options();
   options("tracewriter-dir", bpo::value<std::filesystem::path>()->default_value("tracewriter"),
           "the location of the tracewriter output directory (absolute path or relative to application data dir)");
   options("tracewriter-blocks-per-slice", bpo::value<uint32_t>()->default_value(DEFAULT_BLOCKS_PER_SLICE),
           "number of blocks per slice file (default: 10000)");
   options("tracewriter-log-perf", bpo::bool_switch()->default_value(false),
           "log performance statistics periodically (requires tracewriter_perf logger)");
}

void tracewriter_plugin::plugin_initialize(const variables_map& options) {
   handle_sighup();
   my->plugin_initialize(options);
}

void tracewriter_plugin::plugin_startup() { my->plugin_startup(); }

void tracewriter_plugin::plugin_shutdown() { my->plugin_shutdown(); }

void tracewriter_plugin::handle_sighup() {
   fc::logger::update(logger_name, _tracewriter_log);
   fc::logger::update(logger_perf_name, _tracewriter_perf_log);
   fc::logger::update(logger_io_name, _tracewriter_io_log);
}

} // namespace eosio
