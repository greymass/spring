#include <eosio/roborovski_writer_plugin/abi_json.hpp>
#include <eosio/roborovski_writer_plugin/block_builder.hpp>
#include <eosio/roborovski_writer_plugin/buffer_log.hpp>
#include <eosio/roborovski_writer_plugin/fault.hpp>
#include <eosio/roborovski_writer_plugin/progress.hpp>
#include <eosio/roborovski_writer_plugin/roborovski_writer_plugin.hpp>
#include <eosio/roborovski_writer_plugin/store.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/resource_monitor_plugin/resource_monitor_plugin.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>

namespace eosio {
using namespace chain;
using namespace roborovski_writer;
using boost::signals2::scoped_connection;

static auto _roborovski_writer_plugin = application::register_plugin<roborovski_writer_plugin>();

const std::string logger_name("roborovski_writer");
fc::logger        _roborovski_writer_log;

const std::string logger_perf_name("roborovski_writer_perf");
fc::logger        _roborovski_writer_perf_log;

constexpr uint32_t REPLAY_COMMIT_BLOCKS = 1000;
constexpr size_t   PENDING_WARN_HEIGHTS = 7200;
constexpr size_t   RECENT_RING_SIZE     = 1000;
constexpr uint64_t MIB                  = 1024ull * 1024ull;

namespace {

int64_t now_us() {
   return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
       .count();
}

hash_bytes to_hash(const fc::sha256& id) {
   hash_bytes h;
   std::memcpy(h.data(), id.data(), 32);
   return h;
}

std::string block_range(uint32_t from, uint32_t to) {
   return from == to ? std::to_string(from) : std::to_string(from) + " to " + std::to_string(to);
}

} // namespace

struct writer_metrics {
   std::atomic<uint32_t> notified_block{0};
   std::atomic<uint32_t> prepared_block{0};
   std::atomic<uint32_t> committed_block{0};
   std::atomic<double>   notify_to_commit_seconds{0};
   std::atomic<double>   fsync_seconds{0};
   std::atomic<uint64_t> failures_total{0};
   std::atomic<uint64_t> pending_blocks{0};
   std::atomic<uint64_t> pending_bytes{0};
   std::atomic<uint64_t> free_core{0};
   std::atomic<uint64_t> free_abi{0};
   std::atomic<uint64_t> free_buffer{0};

   struct recent_entry {
      uint32_t block        = 0;
      int64_t  notified_us  = 0;
      int64_t  committed_us = 0;
   };
   std::mutex               recent_mutex;
   std::deque<recent_entry> recent;

   std::string prometheus_text() {
      std::string out;
      auto metric = [&](const char* name, const char* type, const std::string& value) {
         out += std::string("# TYPE roborovski_writer_") + name + " " + type + "\n";
         out += std::string("roborovski_writer_") + name + " " + value + "\n";
      };
      metric("notified_block", "gauge", std::to_string(notified_block.load()));
      metric("prepared_block", "gauge", std::to_string(prepared_block.load()));
      metric("committed_block", "gauge", std::to_string(committed_block.load()));
      metric("notify_to_commit_seconds", "gauge", std::to_string(notify_to_commit_seconds.load()));
      metric("fsync_seconds", "gauge", std::to_string(fsync_seconds.load()));
      metric("failures_total", "counter", std::to_string(failures_total.load()));
      metric("pending_blocks", "gauge", std::to_string(pending_blocks.load()));
      metric("pending_bytes", "gauge", std::to_string(pending_bytes.load()));
      out += "# TYPE roborovski_writer_disk_free_bytes gauge\n";
      out += "roborovski_writer_disk_free_bytes{filesystem=\"core\"} " + std::to_string(free_core.load()) + "\n";
      out += "roborovski_writer_disk_free_bytes{filesystem=\"abi\"} " + std::to_string(free_abi.load()) + "\n";
      out += "roborovski_writer_disk_free_bytes{filesystem=\"buffer\"} " + std::to_string(free_buffer.load()) + "\n";
      return out;
   }

   fc::variant recent_json() {
      std::lock_guard g(recent_mutex);
      fc::variants    rows;
      rows.reserve(recent.size());
      for (const auto& r : recent)
         rows.emplace_back(fc::mutable_variant_object()("block", r.block)("notified_at_us", r.notified_us)(
             "committed_at_us", r.committed_us));
      return fc::variant(rows);
   }
};

struct roborovski_writer_plugin_impl {
   chain_plugin*         chain_plug = nullptr;
   std::filesystem::path core_dir;
   std::filesystem::path abi_dir;
   std::filesystem::path buffer_dir;
   uint64_t              headroom_bytes     = 0;
   uint64_t              buffer_limit_bytes = 0;
   uint32_t              log_interval_s     = 0;
   uint32_t              replay_target      = 0;

   std::optional<scoped_connection> applied_transaction_connection;
   std::optional<scoped_connection> block_start_connection;
   std::optional<scoped_connection> accepted_block_connection;
   std::optional<scoped_connection> irreversible_block_connection;

   std::map<transaction_id_type, cached_trace> cached_traces;
   std::optional<cached_trace>                 onblock_trace;

   std::unique_ptr<roborovski_writer::store> out;
   std::unique_ptr<writer_lock>              buffer_lock;
   std::unique_ptr<buffer_log>               buffer;
   ZSTD_CCtx*                                zstd_ctx = nullptr;

   bool     live           = false;
   bool     failed         = false;
   bool     caught_up      = false;
   bool     needs_seed     = false;
   uint32_t first_block    = 0;
   uint32_t since_commit   = 0;
   size_t   warned_pending = 0;

   std::map<uint32_t, int64_t> notified_at;
   writer_metrics              metrics;

   struct perf_counts {
      uint64_t blocks       = 0;
      uint64_t actions      = 0;
      uint64_t transactions = 0;
      uint64_t bytes        = 0;
      uint64_t glob         = 0;
   };
   perf_counts                           perf_published;
   perf_counts                           perf_committed;
   std::chrono::steady_clock::duration   perf_writer_time{};
   std::chrono::steady_clock::time_point perf_start;

   ~roborovski_writer_plugin_impl() {
      if (zstd_ctx)
         ZSTD_freeCCtx(zstd_ctx);
   }

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();
   void register_http();

   void on_applied_transaction(const transaction_trace_ptr& trace, const packed_transaction_ptr& ptrx);
   void on_block_start(uint32_t block_num);
   void on_accepted_block(const signed_block_ptr& block, const block_id_type& id);
   void on_irreversible_block(const signed_block_ptr& block, const block_id_type& id);

   void     catch_up();
   void     seed_abi_cache();
   void     publish(uint32_t block_num, const block_id_type& id);
   void     verify_committed(uint32_t block_num, const block_id_type& id);
   void     commit();
   void     check_headroom();
   void     update_first_block();
   uint32_t committed_head() const { return out->committed().head; }
      void     log_progress();

   template <typename F>
   void guarded(const char* where, F&& f);
};

template <typename F>
void roborovski_writer_plugin_impl::guarded(const char* where, F&& f) {
   if (failed)
      EOS_THROW(chain::controller_emit_signal_exception, "Roborovski writer stopped after an earlier failure");
   std::string reason;
   const auto  started = std::chrono::steady_clock::now();
   try {
      f();
      perf_writer_time += std::chrono::steady_clock::now() - started;
      return;
   } catch (const fc::exception& e) {
      reason = e.to_detail_string();
   } catch (const std::exception& e) {
      reason = e.what();
   }
   failed = true;
   metrics.failures_total++;
   fc_elog(_roborovski_writer_log, "Roborovski writer failed in ${w}: ${e}", ("w", where)("e", reason));
   app().quit();
   EOS_THROW(chain::controller_emit_signal_exception, "Roborovski writer cannot continue: ${e}", ("e", reason));
}

void roborovski_writer_plugin_impl::plugin_initialize(const variables_map& options) {
   chain_plug = app().find_plugin<chain_plugin>();
   EOS_ASSERT(chain_plug, chain::missing_chain_plugin_exception, "chain_plugin required");
   auto& chain = chain_plug->chain();

   auto resolve = [](std::filesystem::path p) { return p.is_relative() ? app().data_dir() / p : p; };
   core_dir     = resolve(options.at("roborovski-writer-dir").as<std::filesystem::path>());
   abi_dir      = options.count("roborovski-writer-abi-dir")
                      ? resolve(options.at("roborovski-writer-abi-dir").as<std::filesystem::path>())
                      : core_dir / "abis";
   buffer_dir   = options.count("roborovski-writer-buffer-dir")
                      ? resolve(options.at("roborovski-writer-buffer-dir").as<std::filesystem::path>())
                      : core_dir / "buffer";
   headroom_bytes     = options.at("roborovski-writer-headroom-mb").as<uint64_t>() * MIB;
   buffer_limit_bytes = options.at("roborovski-writer-buffer-limit-mb").as<uint64_t>() * MIB;
   log_interval_s     = options.at("roborovski-writer-log-interval").as<uint32_t>();
   if (log_interval_s > 0)
      replay_target = block_log_end(options.count("blocks-dir")
                                        ? resolve(options.at("blocks-dir").as<std::filesystem::path>())
                                        : app().data_dir() / "blocks");

   try {
      configure_fault_hook(std::getenv("ROBOROVSKI_WRITER_FAULT"), _roborovski_writer_log);
      out = std::make_unique<roborovski_writer::store>(core_dir, abi_dir, to_hash(chain.get_chain_id()),
                                                       _roborovski_writer_log);
      buffer_lock = std::make_unique<writer_lock>(buffer_dir);
      buffer      = std::make_unique<buffer_log>(buffer_dir, out->core_binding().core_uuid, out->committed().epoch);
      buffer->discard_through(committed_head());
      check_headroom();
   } catch (const fc::exception&) {
      throw;
   } catch (const std::exception& e) {
      EOS_THROW(chain::plugin_config_exception, "Roborovski writer cannot open its storage: ${e}", ("e", e.what()));
   }

   zstd_ctx = ZSTD_createCCtx();
   EOS_ASSERT(zstd_ctx, chain::plugin_config_exception, "Roborovski writer cannot create a zstd context");

   const auto& scan = buffer->scan_report();
   fc_ilog(_roborovski_writer_log,
           "Roborovski writer core storage: ${c}, ABI cache: ${a}, buffer: ${b}, committed block: ${h}, "
           "buffered entries: ${e}, buffer bytes truncated: ${t}",
           ("c", core_dir.string())("a", abi_dir.string())("b", buffer_dir.string())("h", committed_head())(
               "e", scan.entries)("t", scan.bytes_truncated));

   const auto& m = out->committed();
   needs_seed    = m.head == 0 && m.abi_index_len == 0 && m.abi_floor == 0;
   update_first_block();
   metrics.committed_block = committed_head();

   chain.set_disable_replay_opts(true);
   fc_ilog(_roborovski_writer_log, "Roborovski writer disables replay optimizations so every block emits its traces");

   if (auto resmon = app().find_plugin<resource_monitor_plugin>()) {
      resmon->monitor_directory(core_dir);
      resmon->monitor_directory(abi_dir);
      resmon->monitor_directory(buffer_dir);
   }

   perf_start = std::chrono::steady_clock::now();

   applied_transaction_connection.emplace(chain.applied_transaction().connect(
       [this](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
          const auto started = std::chrono::steady_clock::now();
          on_applied_transaction(std::get<0>(t), std::get<1>(t));
          perf_writer_time += std::chrono::steady_clock::now() - started;
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

   register_http();
}

void roborovski_writer_plugin_impl::register_http() {
   auto& http = app().get_plugin<http_plugin>();
   http.add_async_handler({"/v1/roborovski_writer/metrics", api_category::prometheus,
                           [this](std::string&&, std::string&&, url_response_callback&& cb) {
                              cb(200, fc::variant(metrics.prometheus_text()));
                           }},
                          http_content_type::plaintext);
   http.add_async_handler({"/v1/roborovski_writer/recent", api_category::chain_ro,
                           [this](std::string&&, std::string&&, url_response_callback&& cb) {
                              cb(200, metrics.recent_json());
                           }});
}

void roborovski_writer_plugin_impl::plugin_startup() {
   if (failed)
      return;
   try {
      catch_up();
      commit();
   } catch (const std::exception& e) {
      failed = true;
      metrics.failures_total++;
      fc_elog(_roborovski_writer_log, "Roborovski writer failed at startup: ${e}", ("e", e.what()));
      throw;
   }
   live = true;
   fc_ilog(_roborovski_writer_log, "Roborovski writer started live at committed block ${b}", ("b", committed_head()));
}

void roborovski_writer_plugin_impl::plugin_shutdown() {
   applied_transaction_connection.reset();
   block_start_connection.reset();
   accepted_block_connection.reset();
   irreversible_block_connection.reset();
   if (!out)
      return;
   try {
      if (!failed)
         commit();
      fc_ilog(_roborovski_writer_log, "Roborovski writer stopped at committed block ${b}", ("b", committed_head()));
   } catch (const std::exception& e) {
      fc_elog(_roborovski_writer_log, "Roborovski writer failed its shutdown commit: ${e}", ("e", e.what()));
   }
   try {
      out->close();
   } catch (const std::exception& e) {
      fc_elog(_roborovski_writer_log, "Roborovski writer failed to close its storage: ${e}", ("e", e.what()));
   }
}

void roborovski_writer_plugin_impl::update_first_block() {
   const auto& m = out->committed();
   first_block   = m.head > 0 ? 0 : std::max<uint32_t>(m.abi_floor, 1) + 1;
}

void roborovski_writer_plugin_impl::check_headroom() {
   auto check = [&](const std::filesystem::path& dir, std::atomic<uint64_t>& gauge, const char* name) {
      const uint64_t free = free_bytes(dir);
      gauge               = free;
      if (free < headroom_bytes)
         throw io_exception(std::string("the ") + name + " filesystem at " + dir.string() + " has " +
                            std::to_string(free / MIB) + " MiB free, below the " +
                            std::to_string(headroom_bytes / MIB) + " MiB reserve");
   };
   check(core_dir, metrics.free_core, "core storage");
   check(abi_dir, metrics.free_abi, "ABI cache");
   check(buffer_dir, metrics.free_buffer, "buffer");
}

void roborovski_writer_plugin_impl::catch_up() {
   if (caught_up)
      return;
   caught_up = true;
   if (needs_seed)
      return;

   auto&          chain = chain_plug->chain();
   const uint32_t root  = chain.fork_db_has_root() ? chain.fork_db_root().block_num() : chain.head().block_num();
   const uint32_t from  = std::max(committed_head() + 1, first_block);
   if (root < from)
      return;
   fc_ilog(_roborovski_writer_log, "Nodeos is irreversible through block ${r}; publishing blocks ${f} from the buffer",
           ("r", root)("f", block_range(from, root)));
   for (uint32_t n = from; n <= root; ++n) {
      auto id = chain.chain_block_id_for_num(n);
      if (!id)
         throw data_corruption_exception("nodeos is irreversible through block " + std::to_string(root) +
                                         " but cannot name block " + std::to_string(n) +
                                         "; restore from a snapshot at or below block " +
                                         std::to_string(committed_head()) + " and replay");
      publish(n, *id);
      if (log_interval_s > 0)
         if (auto b = chain.fetch_block_by_number(n))
            perf_published.transactions += stored_transaction_count(*b);
   }
   commit();
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
   catch_up();
   if (needs_seed)
      seed_abi_cache();
   cached_traces.clear();
   onblock_trace.reset();
}

void roborovski_writer_plugin_impl::seed_abi_cache() {
   needs_seed           = false;
   auto&          chain = chain_plug->chain();
   const uint32_t head  = chain.head().block_num();
   const auto&    idx   = chain.db().get_index<account_index, by_name>();
   auto&          abis  = out->abis();
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
      abis.write({head, account.name.to_uint64_t(), std::move(*json)});
      ++count;
   }
   out->set_abi_floor(head > 1 ? head : 0);
   fault_point("abi_initialized", head);
   check_headroom();
   out->commit();
   update_first_block();
   fc_ilog(_roborovski_writer_log,
           "Seeded the ABI cache with ${n} contract ABIs at block ${b}; history starts at block ${f}",
           ("n", count)("b", head)("f", first_block));
}

void roborovski_writer_plugin_impl::on_accepted_block(const signed_block_ptr& block, const block_id_type& id) {
   catch_up();
   const uint32_t block_num = block->block_num();
   if (block_num <= out->written_head() || block_num < first_block || needs_seed) {
      cached_traces.clear();
      onblock_trace.reset();
      return;
   }

   auto built = build_block(*block, id, onblock_trace, cached_traces);
   cached_traces.clear();
   onblock_trace.reset();

   prepared_block p;
   p.block_num    = built.block_num;
   p.id           = id;
   p.parent       = block->previous;
   p.glob_min     = built.glob_min;
   p.glob_max     = built.glob_max;
   p.action_count = built.action_count;
   p.raw_size     = built.blob.size();
   p.abis         = std::move(built.abis);
   {
      std::vector<char> c(ZSTD_compressBound(built.blob.size()));
      size_t n = ZSTD_compressCCtx(zstd_ctx, c.data(), c.size(), built.blob.data(), built.blob.size(), ZSTD_LEVEL);
      if (ZSTD_isError(n))
         throw compression_exception(std::string("zstd compression failed: ") + ZSTD_getErrorName(n));
      c.resize(n);
      p.compressed = std::move(c);
   }

   check_headroom();
   if (buffer->disk_bytes() + p.compressed.size() > buffer_limit_bytes)
      throw io_exception("the buffer holds " + std::to_string(buffer->disk_bytes() / MIB) +
                         " MiB awaiting irreversibility, above the " + std::to_string(buffer_limit_bytes / MIB) +
                         " MiB bound; nodeos has not advanced finality past block " + std::to_string(committed_head()));
   buffer->append(p, live);
   fault_point("buffer_synced", block_num);

   metrics.prepared_block = block_num;
   metrics.pending_bytes  = buffer->disk_bytes();
   const size_t pending   = block_num > committed_head() ? block_num - committed_head() : 0;
   metrics.pending_blocks = pending;
   if (pending >= PENDING_WARN_HEIGHTS && pending >= warned_pending + PENDING_WARN_HEIGHTS / 4) {
      warned_pending = pending;
      fc_wlog(_roborovski_writer_log, "${n} blocks await irreversibility above committed block ${h}",
              ("n", pending)("h", committed_head()));
   } else if (pending < PENDING_WARN_HEIGHTS) {
      warned_pending = 0;
   }
}

void roborovski_writer_plugin_impl::on_irreversible_block(const signed_block_ptr& block, const block_id_type& id) {
   catch_up();
   const uint32_t block_num = block->block_num();
   metrics.notified_block   = block_num;

   if (block_num < first_block)
      return;
   if (block_num <= out->written_head()) {
      verify_committed(block_num, id);
      return;
   }
   notified_at[block_num] = now_us();
   publish(block_num, id);
   if (log_interval_s > 0)
      perf_published.transactions += stored_transaction_count(*block);
   if (live || ++since_commit >= REPLAY_COMMIT_BLOCKS)
      commit();
   if (log_interval_s > 0 && std::chrono::steady_clock::now() - perf_start >= std::chrono::seconds(log_interval_s))
      log_progress();
}

void roborovski_writer_plugin_impl::verify_committed(uint32_t block_num, const block_id_type& id) {
   const auto want = to_hash(id);
   hash_bytes have;
   if (block_num == out->written_head())
      have = out->written_head_id();
   else if (block_num <= committed_head())
      have = read_stored_block_id(core_dir, block_num);
   else
      throw data_corruption_exception("irreversible block " + std::to_string(block_num) +
                                      " arrived again below the written head " + std::to_string(out->written_head()) +
                                      " before it was committed");
   if (have != want)
      throw data_corruption_exception("block " + std::to_string(block_num) + " is " + id.str() +
                                      " in nodeos but " + hex(have.data(), 32) +
                                      " in committed history; stop and repair core storage");
}

void roborovski_writer_plugin_impl::publish(uint32_t block_num, const block_id_type& id) {
   const uint32_t head = out->written_head();
   if (head > 0 && block_num > head + 1)
      throw data_corruption_exception("core storage ends at block " + std::to_string(head) +
                                      " but nodeos made block " + std::to_string(block_num) +
                                      " irreversible; blocks " + block_range(head + 1, block_num - 1) +
                                      " are missing, restore from a snapshot at or below block " +
                                      std::to_string(head));
   if (head == 0 && block_num > first_block)
      throw data_corruption_exception("history starts at block " + std::to_string(first_block) +
                                      " but nodeos made block " + std::to_string(block_num) +
                                      " irreversible; blocks " + block_range(first_block, block_num - 1) +
                                      " are missing, restore from the snapshot the ABI cache was seeded from");
   auto ref = buffer->find(block_num, id);
   if (!ref)
      throw data_corruption_exception("irreversible block " + std::to_string(block_num) + " " + id.str() +
                                      " is in neither committed storage nor the buffer; restore from a snapshot at "
                                      "or below block " + std::to_string(committed_head()) + " and replay");
   if (head > 0 && to_hash(ref->parent) != out->written_head_id())
      throw data_corruption_exception("buffered block " + std::to_string(block_num) +
                                      " does not descend from block " + std::to_string(head) + " " +
                                      hex(out->written_head_id().data(), 32));
   const auto block = buffer->read(block_num, *ref);
   out->append(block);

   perf_published.blocks += 1;
   perf_published.actions += block.action_count;
   perf_published.bytes += block.raw_size;
   perf_published.glob = std::max(perf_published.glob, block.glob_max);
}

void roborovski_writer_plugin_impl::commit() {
   const uint32_t before = committed_head();
   check_headroom();
   if (!out->commit())
      return;
   since_commit            = 0;
   perf_committed.blocks += perf_published.blocks;
   perf_committed.actions += perf_published.actions;
   perf_committed.transactions += perf_published.transactions;
   perf_committed.bytes += perf_published.bytes;
   perf_committed.glob = std::max(perf_committed.glob, perf_published.glob);
   perf_published      = {.glob = perf_published.glob};
   const auto&   m         = out->committed();
   const int64_t done      = now_us();
   metrics.committed_block = m.head;
   metrics.fsync_seconds   = out->last_fsync_seconds();
   if (auto it = notified_at.find(m.head); it != notified_at.end())
      metrics.notify_to_commit_seconds = static_cast<double>(done - it->second) / 1e6;
   {
      std::lock_guard g(metrics.recent_mutex);
      for (auto it = notified_at.upper_bound(before); it != notified_at.end() && it->first <= m.head; ++it) {
         metrics.recent.push_back({it->first, it->second, done});
         if (metrics.recent.size() > RECENT_RING_SIZE)
            metrics.recent.pop_front();
      }
   }
   notified_at.erase(notified_at.begin(), notified_at.upper_bound(m.head));

   if (buffer->discard_through(m.head) > 0)
      fault_point("segment_reclaimed", m.head);
   metrics.pending_bytes  = buffer->disk_bytes();
   metrics.pending_blocks = buffer->highest() > m.head ? buffer->highest() - m.head : 0;
}

void roborovski_writer_plugin_impl::log_progress() {
   const auto   now  = std::chrono::steady_clock::now();
   const auto   head = committed_head();
   const double secs = std::chrono::duration<double>(now - perf_start).count();
   progress_sample s;
   s.committed    = head;
   s.target       = std::max(live ? metrics.notified_block.load() : replay_target, head);
   s.elapsed_s    = secs;
   s.writer_s     = std::chrono::duration<double>(perf_writer_time).count();
   s.blocks       = perf_committed.blocks;
   s.actions      = perf_committed.actions;
   s.transactions = perf_committed.transactions;
   s.bytes        = perf_committed.bytes;
   s.glob         = perf_committed.glob;
   fc_ilog(_roborovski_writer_perf_log, "${line}", ("line", format_progress(s)));
   perf_committed   = {.glob = perf_committed.glob};
   perf_writer_time = {};
   perf_start       = now;
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
   options("roborovski-writer-buffer-dir", bpo::value<std::filesystem::path>(),
           "the location of the buffer that holds accepted blocks until they are irreversible and committed "
           "(absolute path or relative to application data dir); defaults to the buffer directory inside core storage");
   options("roborovski-writer-headroom-mb", bpo::value<uint64_t>()->default_value(4096),
           "free space in MiB that must remain on each filesystem the writer uses; nodeos stops below it");
   options("roborovski-writer-buffer-limit-mb", bpo::value<uint64_t>()->default_value(16384),
           "the most buffered output in MiB awaiting irreversibility; nodeos stops above it");
   options("roborovski-writer-log-interval", bpo::value<uint32_t>()->default_value(0),
           "seconds between ingestion rate lines on the roborovski_writer_perf logger, in coreindex's sync line "
           "format; 0 turns them off");
}

void roborovski_writer_plugin::plugin_initialize(const variables_map& options) {
   handle_sighup();
   my->plugin_initialize(options);
}

void roborovski_writer_plugin::plugin_startup() { my->plugin_startup(); }

void roborovski_writer_plugin::plugin_shutdown() { my->plugin_shutdown(); }

void roborovski_writer_plugin::handle_sighup() {
   fc::logger::update(logger_name, _roborovski_writer_log);
   fc::logger::update(logger_perf_name, _roborovski_writer_perf_log);
}

} // namespace eosio
