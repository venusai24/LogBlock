# Python script tests. Label "scripts", not "unit", so the TSan run
# (ctest -L unit) skips them. Both tests only need a Python interpreter.
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  # Each test file runs itself (unittest.main). -I ignores PYTHONPATH and the
  # current directory, and the files find their inputs from their own location,
  # so ctest can run from any working directory.
  foreach(name IN ITEMS bench_csv bench_jobs)
    add_test(NAME scripts.${name}
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.py)
    set_tests_properties(scripts.${name} PROPERTIES LABELS scripts)
  endforeach()
else()
  message(WARNING "python3 not found: script tests (label 'scripts') are not registered")
endif()
