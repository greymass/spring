add_executable( test_tracewriter_plugin
   test_block_serializer.cpp
   test_slice_writer.cpp
   main.cpp
)
target_link_libraries( test_tracewriter_plugin tracewriter_plugin Boost::included_unit_test_framework )

add_p_test(NAME test_tracewriter_plugin COMMAND plugins/tracewriter_plugin/test/test_tracewriter_plugin)
