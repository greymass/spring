add_executable( test_roborovski_writer_plugin
   test_abi_json.cpp
   test_block_builder.cpp
   test_slice_writer.cpp
   main.cpp
)
target_link_libraries( test_roborovski_writer_plugin roborovski_writer_plugin Boost::included_unit_test_framework )

add_p_test(NAME test_roborovski_writer_plugin COMMAND plugins/roborovski_writer_plugin/test/test_roborovski_writer_plugin)
