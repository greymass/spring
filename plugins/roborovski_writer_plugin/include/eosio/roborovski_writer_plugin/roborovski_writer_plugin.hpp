#pragma once
#include <eosio/chain/application.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>

namespace eosio {

class roborovski_writer_plugin : public plugin<roborovski_writer_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin))

   roborovski_writer_plugin();
   virtual ~roborovski_writer_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

   void handle_sighup() override;

private:
   unique_ptr<struct roborovski_writer_plugin_impl> my;
};

} // namespace eosio
