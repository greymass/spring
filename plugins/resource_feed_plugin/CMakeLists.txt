file(GLOB HEADERS "include/eosio/resource_feed_plugin/*.hpp")
add_library( resource_feed_plugin
             resource_feed_plugin.cpp
             collector.cpp
             snapshot_walker.cpp
             feed_server.cpp
             ${HEADERS} )

target_link_libraries( resource_feed_plugin chain_plugin eosio_chain appbase fc )
target_include_directories( resource_feed_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( test )
