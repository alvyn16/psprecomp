# Verify an installed prefix is usable without the build tree.

foreach(path
    "${PREFIX}/bin/allegrexrecomp"
    "${PREFIX}/lib/libpsprecomp.a"
    "${PREFIX}/lib/liballegrex_core.a"
    "${PREFIX}/include/psprecomp/cpu.h"
    "${PREFIX}/include/psprecomp/decode.h"
    "${PREFIX}/lib/pkgconfig/psprecomp.pc")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "missing installed file: ${path}")
  endif()
endforeach()

file(WRITE "${PREFIX}/consumer.c"
"#include <psprecomp/cpu.h>\n#include <stdio.h>\nint main(void) { psp_cpu_reset(); printf(\"consumer ok\\n\"); return 0; }\n")

# A sanitizer build installs instrumented archives, so the consumer must be
# compiled with the same instrumentation.
if(CFLAGS)
  separate_arguments(CFLAGS_LIST NATIVE_COMMAND "${CFLAGS}")
else()
  set(CFLAGS_LIST)
endif()

execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -std=c11 ${CFLAGS_LIST}
          "-I${PREFIX}/include" "${PREFIX}/consumer.c"
          "-L${PREFIX}/lib" -lpsprecomp -lallegrex_core -lm
          -o "${PREFIX}/consumer"
  RESULT_VARIABLE cc_result OUTPUT_VARIABLE cc_out ERROR_VARIABLE cc_err)
if(NOT cc_result EQUAL 0)
  message(FATAL_ERROR "consumer did not compile: ${cc_out}\n${cc_err}")
endif()

execute_process(COMMAND "${PREFIX}/consumer"
  RESULT_VARIABLE run_result OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer did not run: ${run_out}\n${run_err}")
endif()
message(STATUS "${run_out}")

find_program(PKG_CONFIG NAMES pkg-config)
if(PKG_CONFIG)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig"
            ${PKG_CONFIG} --cflags --libs psprecomp
    RESULT_VARIABLE pc_result OUTPUT_VARIABLE pc_out ERROR_VARIABLE pc_err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT pc_result EQUAL 0)
    message(FATAL_ERROR "pkg-config could not describe psprecomp: ${pc_err}")
  endif()
  if(NOT pc_out MATCHES "-lpsprecomp" OR NOT pc_out MATCHES "include")
    message(FATAL_ERROR "unexpected pkg-config flags: ${pc_out}")
  endif()
  message(STATUS "pkg-config: ${pc_out}")
endif()
