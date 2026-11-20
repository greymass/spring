file(GLOB HEADERS "include/eosio/roborovski_writer_plugin/*.hpp")
add_library( roborovski_writer_plugin
             roborovski_writer_plugin.cpp
             block_builder.cpp
             abi_json.cpp
             abi_writer.cpp
             slice_writer.cpp
             ${HEADERS} )

find_package(zstd REQUIRED)
target_link_libraries( roborovski_writer_plugin chain_plugin resource_monitor_plugin eosio_chain appbase fc zstd::libzstd_shared )
target_include_directories( roborovski_writer_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( test )
