#pragma once
#include <eosio/chain/application.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>

namespace eosio {

class resource_feed_plugin : public plugin<resource_feed_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin))

   resource_feed_plugin();
   virtual ~resource_feed_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

   void handle_sighup() override;

private:
   unique_ptr<struct resource_feed_plugin_impl> my;
};

} // namespace eosio
