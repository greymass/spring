file(GLOB HEADERS "include/eosio/roborovski_writer_plugin/*.hpp")
add_library( roborovski_writer_plugin
             roborovski_writer_plugin.cpp
             block_builder.cpp
             abi_json.cpp
             abi_writer.cpp
             slice_writer.cpp
             store_format.cpp
             recovery.cpp
             buffer_log.cpp
             fault.cpp
             store.cpp
             ${HEADERS} )

find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
   set(ROBOROVSKI_ZSTD zstd::libzstd_shared)
else()
   find_package(PkgConfig REQUIRED)
   pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
   set(ROBOROVSKI_ZSTD PkgConfig::ZSTD)
endif()
target_link_libraries( roborovski_writer_plugin chain_plugin http_plugin resource_monitor_plugin eosio_chain appbase fc ${ROBOROVSKI_ZSTD} )
target_include_directories( roborovski_writer_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" )

add_subdirectory( test )
