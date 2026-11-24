add_executable( test_roborovski_writer_plugin
   test_abi_json.cpp
   test_block_builder.cpp
   test_slice_writer.cpp
   test_buffer_log.cpp
   test_store.cpp
   test_progress.cpp
   main.cpp
)
target_link_libraries( test_roborovski_writer_plugin roborovski_writer_plugin Boost::included_unit_test_framework )
target_compile_definitions( test_roborovski_writer_plugin PRIVATE ROBOROVSKI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures" )

add_p_test(NAME test_roborovski_writer_plugin COMMAND plugins/roborovski_writer_plugin/test/test_roborovski_writer_plugin)
