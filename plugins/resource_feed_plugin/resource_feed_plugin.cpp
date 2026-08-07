#include <eosio/resource_feed_plugin/resource_feed_plugin.hpp>

#include <eosio/resource_feed_plugin/collector.hpp>
#include <eosio/resource_feed_plugin/feed_server.hpp>
#include <eosio/resource_feed_plugin/serializer.hpp>
#include <eosio/resource_feed_plugin/snapshot_walker.hpp>

#include <eosio/chain/exceptions.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>

#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace eosio {
using namespace chain;
using boost::signals2::scoped_connection;

static auto _resource_feed_plugin = application::register_plugin<resource_feed_plugin>();

const std::string logger_name("resource_feed");
fc::logger        _resource_feed_log;

constexpr uint32_t DEFAULT_CHUNK_SIZE          = 10000;
constexpr uint32_t DEFAULT_MAX_QUEUE_MB        = 64;
constexpr uint32_t DEFAULT_HEAD_THRESHOLD_SEC  = 300;

struct resource_feed_plugin_impl {
private:
   chain_plugin*         chain_plug = nullptr;
   std::filesystem::path socket_path;
   uint32_t              chunk_size         = 0;
   uint32_t              max_queue_mb       = 0;
   uint32_t              head_threshold_sec = 0;

   std::unique_ptr<resource_feed::feed_server>          server;
   std::map<uint64_t, resource_feed::snapshot_walker>   walkers;

   std::optional<scoped_connection> accepted_block_connection;

public:
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

   void on_accepted_block(const signed_block_ptr& block, const block_id_type& id);
};

void resource_feed_plugin_impl::plugin_initialize(const variables_map& options) {
   try {
      chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT(chain_plug, chain::missing_chain_plugin_exception, "chain_plugin required");
      auto& chain = chain_plug->chain();

      auto path_option = options.at("resource-feed-socket-path").as<std::filesystem::path>();
      if (path_option.is_relative())
         socket_path = app().data_dir() / path_option;
      else
         socket_path = path_option;

      chunk_size         = options.at("resource-feed-chunk-size").as<uint32_t>();
      max_queue_mb       = options.at("resource-feed-max-queue-mb").as<uint32_t>();
      head_threshold_sec = options.at("resource-feed-head-threshold-sec").as<uint32_t>();

      fc_ilog(_resource_feed_log, "Resource feed socket path: ${p}", ("p", socket_path.string()));

      server = std::make_unique<resource_feed::feed_server>(socket_path,
                                                           static_cast<size_t>(max_queue_mb) * 1024 * 1024);

      accepted_block_connection.emplace(chain.accepted_block().connect([&](const block_signal_params& t) {
         const auto& [block, id] = t;
         on_accepted_block(block, id);
      }));
   }
   FC_LOG_AND_RETHROW()
}

void resource_feed_plugin_impl::plugin_startup() {
   server->start();
   fc_ilog(_resource_feed_log, "Resource feed plugin started");
}

void resource_feed_plugin_impl::plugin_shutdown() {
   fc_ilog(_resource_feed_log, "Resource feed plugin shutting down...");
   accepted_block_connection.reset();
   if (server)
      server->stop();
   walkers.clear();
   fc_ilog(_resource_feed_log, "Resource feed plugin shutdown complete");
}

void resource_feed_plugin_impl::on_accepted_block(const signed_block_ptr& block, const block_id_type& id) {
   try {
      auto& chain = chain_plug->chain();

      if (fc::time_point::now() - block->timestamp.to_time_point() > fc::seconds(head_threshold_sec)) {
         if (!walkers.empty() || !server->clients().empty()) {
            fc_wlog(_resource_feed_log, "Resource feed head is stale at block ${n}; dropping clients",
                    ("n", block->block_num()));
            server->disconnect_all();
            walkers.clear();
         }
         return;
      }

      const auto&    db        = chain.db();
      const uint32_t block_num = block->block_num();

      const auto             deltas = resource_feed::collect_block_deltas(db);
      resource_feed::serializer delta_ser;
      if (!deltas.accounts.empty())
         delta_ser.accounts(block_num, deltas.accounts);
      if (!deltas.removed.empty())
         delta_ser.accounts_removed(block_num, deltas.removed);
      if (deltas.globals)
         delta_ser.globals(block_num, *deltas.globals);
      if (deltas.config)
         delta_ser.config(block_num, *deltas.config);
      const auto& delta_frames = delta_ser.data();

      resource_feed::serializer block_ser;
      block_ser.block(block_num, id, block->timestamp.slot);
      const auto& block_frames = block_ser.data();

      const auto            connected = server->clients();
      std::set<uint64_t>    live;
      for (const auto& c : connected)
         live.insert(c.id);
      for (auto it = walkers.begin(); it != walkers.end();)
         it = live.count(it->first) ? std::next(it) : walkers.erase(it);

      for (const auto& c : connected) {
         resource_feed::serializer ser;

         if (c.needs_start) {
            ser.hello(chain.get_chain_id(), block_num);
            ser.config(block_num, resource_feed::read_config(db));
            ser.globals(block_num, resource_feed::read_globals(db));
            ser.snapshot_begin(block_num);
            walkers[c.id].start(block_num);
            server->mark_started(c.id);
         }

         ser.append(delta_frames);

         auto walker_it = walkers.find(c.id);
         if (walker_it != walkers.end() && walker_it->second.active()) {
            ser.accounts(block_num, walker_it->second.next_chunk(db, chunk_size));
            if (!walker_it->second.active()) {
               ser.snapshot_end(block_num);
               walkers.erase(walker_it);
            }
         }

         ser.append(block_frames);

         server->send(c.id, ser.take());
      }
   } catch (const fc::exception& e) {
      fc_elog(_resource_feed_log, "Resource feed failed on block: ${e}", ("e", e.to_detail_string()));
   } catch (const std::exception& e) {
      fc_elog(_resource_feed_log, "Resource feed failed on block: ${e}", ("e", e.what()));
   }
}

resource_feed_plugin::resource_feed_plugin()
   : my(new resource_feed_plugin_impl()) {}

resource_feed_plugin::~resource_feed_plugin() = default;

void resource_feed_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto options = cfg.add_options();
   options("resource-feed-socket-path", bpo::value<std::filesystem::path>()->default_value("resource_feed.sock"),
           "the location of the resource feed unix socket (absolute path or relative to application data dir)");
   options("resource-feed-chunk-size", bpo::value<uint32_t>()->default_value(DEFAULT_CHUNK_SIZE),
           "number of account rows per snapshot chunk (default: 10000)");
   options("resource-feed-max-queue-mb", bpo::value<uint32_t>()->default_value(DEFAULT_MAX_QUEUE_MB),
           "maximum per-client send queue size in megabytes before disconnecting (default: 64)");
   options("resource-feed-head-threshold-sec", bpo::value<uint32_t>()->default_value(DEFAULT_HEAD_THRESHOLD_SEC),
           "seconds behind head after which the feed is considered not caught up (default: 300)");
}

void resource_feed_plugin::plugin_initialize(const variables_map& options) {
   handle_sighup();
   my->plugin_initialize(options);
}

void resource_feed_plugin::plugin_startup() { my->plugin_startup(); }

void resource_feed_plugin::plugin_shutdown() { my->plugin_shutdown(); }

void resource_feed_plugin::handle_sighup() { fc::logger::update(logger_name, _resource_feed_log); }

} // namespace eosio
