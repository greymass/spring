add_executable( test_resource_feed_plugin
   main.cpp
   test_serializer.cpp
   test_collector.cpp
   test_snapshot_walker.cpp
   test_feed_server.cpp
)
target_link_libraries( test_resource_feed_plugin resource_feed_plugin eosio_testing Boost::included_unit_test_framework )

add_p_test(NAME test_resource_feed_plugin COMMAND plugins/resource_feed_plugin/test/test_resource_feed_plugin)
