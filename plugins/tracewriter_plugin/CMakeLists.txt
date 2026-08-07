file(GLOB HEADERS "include/eosio/tracewriter_plugin/*.hpp")
add_library( tracewriter_plugin
             tracewriter_plugin.cpp
             slice_writer.cpp
             ${HEADERS} )

find_package(zstd REQUIRED)
target_link_libraries( tracewriter_plugin chain_plugin eosio_chain appbase fc zstd::libzstd_shared )
target_include_directories( tracewriter_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( test )
