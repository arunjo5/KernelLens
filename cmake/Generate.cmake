if(KIND STREQUAL "btf")
  execute_process(COMMAND "${BPFTOOL}" btf dump file "${INPUT}" format c
    OUTPUT_FILE "${OUTPUT}" RESULT_VARIABLE result ERROR_VARIABLE error)
elseif(KIND STREQUAL "skeleton")
  execute_process(COMMAND "${BPFTOOL}" gen skeleton "${INPUT}" name latency_bpf
    OUTPUT_FILE "${OUTPUT}" RESULT_VARIABLE result ERROR_VARIABLE error)
else()
  message(FATAL_ERROR "Unknown generation kind")
endif()
if(NOT result EQUAL 0)
  file(REMOVE "${OUTPUT}")
  message(FATAL_ERROR "bpftool ${KIND} failed: ${error}")
endif()
