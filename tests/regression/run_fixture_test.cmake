# Runs the fixture regression end-to-end from a ctest command line:
#   1. render the committed fixture scene.pkg through the verifier
#   -P script so it works from any build dir, no shell required.
#
# Invoked by the root CMakeLists.txt fixture_regression test with:
#   -DFIXTURE_PKG_DIR=<...> -DBASELINE_DIR=<...> -DVERIFIER_BIN=<...>
#   -DOUT_DIR=<...> -DPYTHON=<...> -DCOMPARE_SCRIPT=<...>

if(NOT FIXTURE_PKG_DIR OR NOT BASELINE_DIR OR NOT VERIFIER_BIN OR NOT OUT_DIR
   OR NOT PYTHON OR NOT COMPARE_SCRIPT)
    message(FATAL_ERROR "run_fixture_test.cmake: missing required -D arguments")
endif()

if(NOT EXISTS "${VERIFIER_BIN}")
    message(FATAL_ERROR "verifier binary not found: ${VERIFIER_BIN}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

set(ID "fixture_test_scene")

# 1. Render: verifier output dir layout is <OUT_DIR>/<id>.png
#    QT_QPA_PLATFORM comes from the ctest ENVIRONMENT (offscreen).
execute_process(
    COMMAND "${VERIFIER_BIN}" "${FIXTURE_PKG_DIR}" --id "${ID}" --output "${OUT_DIR}"
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_out
    ERROR_VARIABLE render_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT render_result EQUAL 0)
    message(FATAL_ERROR "verifier failed (exit ${render_result})\nstdout: ${render_out}\nstderr: ${render_err}")
endif()
message(STATUS "verifier: ${render_out}")

set(SNAPSHOT "${OUT_DIR}/${ID}.png")
if(NOT EXISTS "${SNAPSHOT}")
    message(FATAL_ERROR "verifier produced no snapshot at ${SNAPSHOT}\nstdout: ${render_out}\nstderr: ${render_err}")
endif()

# 2. Compare against the committed baseline (same thresholds as the
#    png_regression_batch test).
execute_process(
    COMMAND "${PYTHON}" "${COMPARE_SCRIPT}" "${BASELINE_DIR}" "${OUT_DIR}"
    RESULT_VARIABLE cmp_result
    OUTPUT_VARIABLE cmp_out
    ERROR_VARIABLE cmp_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT cmp_result EQUAL 0)
    message(FATAL_ERROR "baseline diff failed (exit ${cmp_result})\nstdout: ${cmp_out}\nstderr: ${cmp_err}")
endif()
message(STATUS "compare: ${cmp_out}")

message(STATUS "fixture_regression: PASS")
