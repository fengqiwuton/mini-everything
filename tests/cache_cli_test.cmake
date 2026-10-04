file(MAKE_DIRECTORY "${TEST_DIR}/root")
file(WRITE "${TEST_DIR}/root/one.txt" "hello")
string(RANDOM LENGTH 12 ALPHABET 0123456789 cache_run)
set(cache_db "${TEST_DIR}/index-${cache_run}.db")
execute_process(COMMAND "${APP}" scan "${TEST_DIR}/root" --db "${cache_db}" --profile
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT error MATCHES "open_ms=" OR NOT error MATCHES "clear_ms=0[.]00" OR
    NOT error MATCHES "db_cache_mib=64" OR NOT error MATCHES "cache_spills=")
    message(FATAL_ERROR "default scan cache/profile missing: ${result} ${output} ${error}")
endif()
execute_process(COMMAND "${APP}" scan "${TEST_DIR}/root" --db "${cache_db}" --db-cache-mib 2 --profile
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "Indexed 1 files" OR NOT error MATCHES "db_cache_mib=2")
    message(FATAL_ERROR "custom scan cache failed: ${result} ${output} ${error}")
endif()
foreach(invalid IN ITEMS 0 -1 1025 2junk 1.5)
    execute_process(COMMAND "${APP}" scan "${TEST_DIR}/root" --db "${TEST_DIR}/invalid.db" --db-cache-mib "${invalid}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 2 OR EXISTS "${TEST_DIR}/invalid.db")
        message(FATAL_ERROR "invalid cache was accepted or touched index: ${invalid} ${result}")
    endif()
endforeach()
execute_process(COMMAND "${APP}" scan "${TEST_DIR}/root" --db "${cache_db}" --db-cache-mib
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "missing cache argument accepted")
endif()
execute_process(COMMAND "${APP}" scan "${TEST_DIR}/root" --db "${cache_db}" --db-cache-mib 2 --db-cache-mib 4
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "duplicate cache argument accepted")
endif()
execute_process(COMMAND "${APP}" search one --db "${cache_db}" --db-cache-mib 2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "scan-only cache argument accepted by search")
endif()
execute_process(COMMAND "${APP}" search one --db "${cache_db}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "one.txt")
    message(FATAL_ERROR "invalid cache options damaged the old index")
endif()
