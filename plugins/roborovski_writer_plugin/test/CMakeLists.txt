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

file(GLOB ROBOROVSKI_INTEGRATION RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/integration" "integration/*.py" "integration/*.sh")
foreach(f ${ROBOROVSKI_INTEGRATION})
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/integration/${f} ${CMAKE_BINARY_DIR}/tests/${f} COPYONLY)
endforeach()

if(DEFINED ENV{GITHUB_ACTIONS})
   set(UNSHARE "--unshared")
else()
   set(UNSHARE "")
endif()

foreach(t crash two_writers restart fork)
   add_lr_test(NAME roborovski_writer_${t}_test COMMAND tests/roborovski_writer_${t}_test.py -v ${UNSHARE})
endforeach()
add_lr_test(NAME roborovski_writer_cutover_test COMMAND tests/roborovski_writer_cutover_test.py -v ${UNSHARE})
set_property(TEST roborovski_writer_cutover_test PROPERTY SKIP_RETURN_CODE 77)
add_lr_test(NAME roborovski_writer_rotation_test COMMAND tests/roborovski_writer_rotation_test.py -v)
set_property(TEST roborovski_writer_rotation_test PROPERTY SKIP_RETURN_CODE 77)
