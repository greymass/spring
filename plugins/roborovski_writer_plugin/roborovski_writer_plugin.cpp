#include <eosio/roborovski_writer_plugin/abi_json.hpp>
#include <eosio/roborovski_writer_plugin/abi_writer.hpp>
#include <eosio/roborovski_writer_plugin/block_builder.hpp>
#include <eosio/roborovski_writer_plugin/roborovski_writer_plugin.hpp>
#include <eosio/roborovski_writer_plugin/slice_writer.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/resource_monitor_plugin/resource_monitor_plugin.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>

#include <chrono>
#include <filesystem>
#include <map>

namespace eosio {
using namespace chain;
using namespace roborovski_writer;
using boost::signals2::scoped_connection;

static auto _roborovski_writer_plugin = application::register_plugin<roborovski_writer_plugin>();

const std::string logger_name("roborovski_writer");
fc::logger        _roborovski_writer_log;

const std::string logger_perf_name("roborovski_writer_perf");
fc::logger        _roborovski_writer_perf_log;

constexpr uint32_t PERF_LOG_INTERVAL = 10000;

struct roborovski_writer_plugin_impl {
   chain_plugin*         chain_plug = nullptr;
   std::filesystem::path core_dir;
   std::filesystem::path abi_dir;
   bool                  log_perf = false;

   std::optional<scoped_connection> applied_transaction_connection;
   std::optional<scoped_connection> block_start_connection;
   std::optional<scoped_connection> accepted_block_connection;
   std::optional<scoped_connection> irreversible_block_connection;

   std::map<transaction_id_type, cached_trace> cached_traces;
   std::optional<cached_trace>                 onblock_trace;
   std::map<uint32_t, built_block>             pending;

   std::unique_ptr<slice_writer> core;
   std::unique_ptr<abi_writer>   abis;
   bool                          seed_abis = false;

   uint64_t                              perf_blocks  = 0;
   uint64_t                              perf_actions = 0;
   uint64_t                              perf_bytes   = 0;
   std::chrono::steady_clock::time_point perf_start;

   void plugin_initialize(const variables_map& options);
   void plugin_shutdown();

   void on_applied_transaction(const transaction_trace_ptr& trace, const packed_transaction_ptr& ptrx);
   void on_block_start(uint32_t block_num);
   void on_accepted_block(const signed_block_ptr& block, const block_id_type& id);
   void on_irreversible_block(const signed_block_ptr& block, const block_id_type& id);

   void seed_abi_cache();
   void write_irreversible(const signed_block_ptr& block, const block_id_type& id);
   void log_perf_stats(uint32_t block_num);

   template <typename F>
   void guarded(const char* where, F&& f);
};

template <typename F>
void roborovski_writer_plugin_impl::guarded(const char* where, F&& f) {
   try {
      f();
   } catch (const std::exception& e) {
      fc_elog(_roborovski_writer_log, "Roborovski writer failed in ${w}: ${e}", ("w", where)("e", e.what()));
      app().quit();
      EOS_THROW(chain::plugin_exception, "Roborovski writer cannot continue: ${e}", ("e", e.what()));
   } catch (const fc::exception& e) {
      fc_elog(_roborovski_writer_log, "Roborovski writer failed in ${w}: ${e}", ("w", where)("e", e.to_detail_string()));
      app().quit();
      EOS_THROW(chain::plugin_exception, "Roborovski writer cannot continue: ${e}", ("e", e.to_string()));
   }
}

void roborovski_writer_plugin_impl::plugin_initialize(const variables_map& options) {
   chain_plug = app().find_plugin<chain_plugin>();
   EOS_ASSERT(chain_plug, chain::missing_chain_plugin_exception, "chain_plugin required");
   auto& chain = chain_plug->chain();

   core_dir = options.at("roborovski-writer-dir").as<std::filesystem::path>();
   if (core_dir.is_relative())
      core_dir = app().data_dir() / core_dir;

   if (options.count("roborovski-writer-abi-dir")) {
      abi_dir = options.at("roborovski-writer-abi-dir").as<std::filesystem::path>();
      if (abi_dir.is_relative())
         abi_dir = app().data_dir() / abi_dir;
   } else {
      abi_dir = core_dir / "abis";
   }

   log_perf = options.at("roborovski-writer-log-perf").as<bool>();

   std::filesystem::create_directories(core_dir);
   std::filesystem::create_directories(abi_dir);
   if (auto resmon = app().find_plugin<resource_monitor_plugin>()) {
      resmon->monitor_directory(core_dir);
      resmon->monitor_directory(abi_dir);
   }

   try {
      core = std::make_unique<slice_writer>(core_dir, _roborovski_writer_log);
      abis = std::make_unique<abi_writer>(abi_dir);
   } catch (const std::exception& e) {
      EOS_THROW(chain::plugin_config_exception, "Roborovski writer cannot open its storage: ${e}", ("e", e.what()));
   }
   seed_abis = core->empty() && abis->empty();

   fc_ilog(_roborovski_writer_log, "Roborovski writer core storage: ${c}, ABI cache: ${a}, last block: ${b}",
           ("c", core_dir.string())("a", abi_dir.string())("b", core->get_last_block()));

   perf_start = std::chrono::steady_clock::now();

   applied_transaction_connection.emplace(chain.applied_transaction().connect(
       [this](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          on_applied_transaction(std::get<0>(t), std::get<1>(t));
       }));
   block_start_connection.emplace(chain.block_start().connect([this](uint32_t block_num) {
      guarded("block start", [&] { on_block_start(block_num); });
   }));
   accepted_block_connection.emplace(chain.accepted_block().connect([this](const block_signal_params& t) {
      const auto& [block, id] = t;
      guarded("accepted block", [&] { on_accepted_block(block, id); });
   }));
   irreversible_block_connection.emplace(chain.irreversible_block().connect([this](const block_signal_params& t) {
      const auto& [block, id] = t;
      guarded("irreversible block", [&] { on_irreversible_block(block, id); });
   }));
}

void roborovski_writer_plugin_impl::plugin_shutdown() {
   applied_transaction_connection.reset();
   block_start_connection.reset();
   accepted_block_connection.reset();
   irreversible_block_connection.reset();
   try {
      if (abis)
         abis->flush();
      if (core) {
         core->close();
         fc_ilog(_roborovski_writer_log, "Roborovski writer stopped at block ${b}", ("b", core->get_last_block()));
      }
   } catch (const std::exception& e) {
      fc_elog(_roborovski_writer_log, "Roborovski writer failed to close its storage: ${e}", ("e", e.what()));
   }
}

void roborovski_writer_plugin_impl::on_applied_transaction(const transaction_trace_ptr&  trace,
                                                           const packed_transaction_ptr& ptrx) {
   if (!trace->receipt)
      return;
   if (trace->receipt->status != transaction_receipt_header::executed &&
       trace->receipt->status != transaction_receipt_header::soft_fail)
      return;
   if (is_onblock(*trace))
      onblock_trace.emplace(cached_trace{trace, ptrx});
   else if (trace->failed_dtrx_trace)
      cached_traces[trace->failed_dtrx_trace->id] = {trace, ptrx};
   else
      cached_traces[trace->id] = {trace, ptrx};
}

void roborovski_writer_plugin_impl::on_block_start(uint32_t block_num) {
   if (seed_abis)
      seed_abi_cache();
   cached_traces.clear();
   onblock_trace.reset();
   pending.erase(pending.lower_bound(block_num), pending.end());
}

void roborovski_writer_plugin_impl::seed_abi_cache() {
   seed_abis = false;
   auto&          chain = chain_plug->chain();
   const uint32_t head  = chain.head().block_num();
   const auto&    idx   = chain.db().get_index<account_index, by_name>();
   size_t         count = 0;
   for (const auto& account : idx) {
      if (account.abi.size() == 0)
         continue;
      auto json = abi_binary_to_json(account.abi.data(), account.abi.size());
      if (!json) {
         fc_wlog(_roborovski_writer_log, "ABI of ${a} at block ${b} cannot be decoded and is not seeded",
                 ("a", account.name)("b", head));
         continue;
      }
      abis->write({head, account.name.to_uint64_t(), std::move(*json)});
      ++count;
   }
   abis->flush();
   fc_ilog(_roborovski_writer_log, "Seeded the ABI cache with ${n} contract ABIs at block ${b}", ("n", count)("b", head));
}

void roborovski_writer_plugin_impl::on_accepted_block(const signed_block_ptr& block, const block_id_type& id) {
   auto built = build_block(*block, id, onblock_trace, cached_traces);
   pending.insert_or_assign(built.block_num, std::move(built));
   cached_traces.clear();
   onblock_trace.reset();
}

void roborovski_writer_plugin_impl::on_irreversible_block(const signed_block_ptr& block, const block_id_type& id) {
   write_irreversible(block, id);
   if (log_perf && perf_blocks >= PERF_LOG_INTERVAL)
      log_perf_stats(block->block_num());
}

void roborovski_writer_plugin_impl::write_irreversible(const signed_block_ptr& block, const block_id_type& id) {
   const uint32_t block_num = block->block_num();
   auto           it        = pending.find(block_num);

   if (!core->empty() && block_num <= core->get_last_block()) {
      if (it != pending.end())
         pending.erase(pending.begin(), std::next(it));
      return;
   }

   if (it == pending.end()) {
      if (core->empty())
         return;
      throw data_corruption_exception("irreversible block " + std::to_string(block_num) +
                                      " was not captured when it was accepted");
   }
   if (it->second.id != id)
      throw data_corruption_exception("irreversible block " + std::to_string(block_num) + " is " + id.str() +
                                      " but the captured block is " + it->second.id.str());

   for (const auto& rec : it->second.abis)
      abis->write(rec);
   abis->flush();

   core->append(it->second);
   core->flush();

   perf_blocks  += 1;
   perf_actions += it->second.action_count;
   perf_bytes   += it->second.blob.size();

   pending.erase(pending.begin(), std::next(it));
}

void roborovski_writer_plugin_impl::log_perf_stats(uint32_t block_num) {
   auto now     = std::chrono::steady_clock::now();
   auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - perf_start).count();
   fc_ilog(_roborovski_writer_perf_log, "block ${b}: ${n} blocks in ${ms} ms, ${a} actions, ${bytes} bytes before compression",
           ("b", block_num)("n", perf_blocks)("ms", wall_ms)("a", perf_actions)("bytes", perf_bytes));
   perf_blocks = perf_actions = perf_bytes = 0;
   perf_start                              = now;
}

roborovski_writer_plugin::roborovski_writer_plugin()
   : my(new roborovski_writer_plugin_impl()) {}

roborovski_writer_plugin::~roborovski_writer_plugin() = default;

void roborovski_writer_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto options = cfg.add_options();
   options("roborovski-writer-dir", bpo::value<std::filesystem::path>()->default_value("roborovski"),
           "the location of Roborovski core storage (absolute path or relative to application data dir)");
   options("roborovski-writer-abi-dir", bpo::value<std::filesystem::path>(),
           "the location of the Roborovski ABI cache (absolute path or relative to application data dir); "
           "defaults to the abis directory inside core storage");
   options("roborovski-writer-log-perf", bpo::bool_switch()->default_value(false),
           "log throughput every 10000 blocks to the roborovski_writer_perf logger");
}

void roborovski_writer_plugin::plugin_initialize(const variables_map& options) {
   handle_sighup();
   my->plugin_initialize(options);
}

void roborovski_writer_plugin::plugin_startup() { fc_ilog(_roborovski_writer_log, "Roborovski writer started"); }

void roborovski_writer_plugin::plugin_shutdown() { my->plugin_shutdown(); }

void roborovski_writer_plugin::handle_sighup() {
   fc::logger::update(logger_name, _roborovski_writer_log);
   fc::logger::update(logger_perf_name, _roborovski_writer_perf_log);
}

} // namespace eosio
