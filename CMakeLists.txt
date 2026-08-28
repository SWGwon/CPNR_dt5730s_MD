cmake_minimum_required(VERSION 3.17)
project(CAEN_DT5730_3TIER_DAQ CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -pthread")
option(CPNR_NATIVE_OPTIMIZATION
       "Build only for the configure host CPU (-march=native)" OFF)
if(CPNR_NATIVE_OPTIMIZATION)
  add_compile_options(-march=native)
endif()

# Re-run configure before a build whenever provenance-relevant source/config or
# the current main ref/index changes. This keeps embedded commit/dirty stamps
# from silently lagging behind a plain `cmake --build` invocation.
file(GLOB_RECURSE CPNR_PROVENANCE_INPUTS CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/src/*" "${CMAKE_SOURCE_DIR}/include/*"
    "${CMAKE_SOURCE_DIR}/gui/*" "${CMAKE_SOURCE_DIR}/config/*"
    "${CMAKE_SOURCE_DIR}/tests/*")
list(FILTER CPNR_PROVENANCE_INPUTS EXCLUDE REGEX
    "/__pycache__/|\\.py[co]$")
list(APPEND CPNR_PROVENANCE_INPUTS
    "${CMAKE_SOURCE_DIR}/README.md"
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/requirements.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CPNR_PROVENANCE_INPUTS}
    "${CMAKE_SOURCE_DIR}/.git/HEAD"
    "${CMAKE_SOURCE_DIR}/.git/index"
    "${CMAKE_SOURCE_DIR}/.git/refs/heads/main"
    "${CMAKE_SOURCE_DIR}/.git/packed-refs")

include(CTest)
option(BUILD_DAQ_APPLICATIONS "Build hardware DAQ applications and deploy the GUI" ON)
option(BUILD_DAQ_FRONTEND "Build the CAEN/ZeroMQ hardware frontend" ON)
option(BUILD_PRODUCTION_CONVERTER "Build the ROOT production converter" ON)
option(BUILD_ROOT_VALIDATOR "Build the read-only production ROOT validator" ON)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(config_parser_tests tests/config_parser_tests.cpp src/DAQConfig.cpp)
  target_include_directories(config_parser_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(config_parser_tests PRIVATE CPNR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(config_parser_tests PRIVATE stdc++fs)
  endif()
  set_target_properties(config_parser_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME config_parser_regression COMMAND config_parser_tests)

  add_executable(caen_digitizer_tests tests/caen_digitizer_tests.cpp)
  target_include_directories(caen_digitizer_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/mocks
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(caen_digitizer_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(caen_digitizer_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME caen_digitizer_lifecycle_regression
           COMMAND caen_digitizer_tests)

  add_executable(trigger_calibration_tests
      tests/trigger_calibration_tests.cpp
      src/TriggerCalibration.cpp
      src/DAQConfig.cpp)
  target_include_directories(trigger_calibration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(trigger_calibration_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME trigger_calibration_regression COMMAND trigger_calibration_tests)

  add_executable(dt5730_timing_tests tests/dt5730_timing_tests.cpp)
  target_include_directories(dt5730_timing_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(dt5730_timing_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(dt5730_timing_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME dt5730_timing_contract_regression
           COMMAND dt5730_timing_tests)

  add_executable(dt5730_status_tests tests/dt5730_status_tests.cpp)
  target_include_directories(dt5730_status_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/mocks
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(dt5730_status_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(dt5730_status_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME dt5730_status_contract_regression
           COMMAND dt5730_status_tests)

  add_executable(data_quality_tests tests/data_quality_tests.cpp)
  target_include_directories(data_quality_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(data_quality_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(data_quality_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME data_quality_policy_regression COMMAND data_quality_tests)

  add_executable(daq_manager_mock_tests
      tests/daq_manager_mock_tests.cpp
      src/DAQManager.cpp
      src/DAQConfig.cpp
      src/TriggerCalibration.cpp
      src/Sha256.cpp)
  target_include_directories(daq_manager_mock_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/mocks
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(daq_manager_mock_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(daq_manager_mock_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME daq_manager_hardware_free_integration
           COMMAND daq_manager_mock_tests)

  add_executable(frontend_hardware_free_compile
      src/frontend_dt5730.cpp
      src/DAQManager.cpp
      src/DAQConfig.cpp
      src/TriggerCalibration.cpp
      src/Sha256.cpp)
  target_include_directories(frontend_hardware_free_compile PRIVATE
      ${CMAKE_SOURCE_DIR}/tests/mocks
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(frontend_hardware_free_compile PRIVATE
      -Wall -Wextra -Werror)
  target_compile_definitions(frontend_hardware_free_compile PRIVATE
      CPNR_GIT_COMMIT="hardware-free-test"
      CPNR_BUILD_TIMESTAMP="hardware-free-test")
  set_target_properties(frontend_hardware_free_compile PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME frontend_cli_and_signal_regression
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_SOURCE_DIR}/tests/frontend_cli_test.py
              $<TARGET_FILE:frontend_hardware_free_compile>)

  add_executable(sha256_tests tests/sha256_tests.cpp src/Sha256.cpp)
  target_include_directories(sha256_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(sha256_tests PRIVATE -Wall -Wextra -Werror)
  set_target_properties(sha256_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME sha256_regression COMMAND sha256_tests)

  add_executable(raw_root_fidelity_tests
      tests/raw_root_fidelity_tests.cpp
      src/RawRootFidelity.cpp
      src/WaveformDsp.cpp
      src/Sha256.cpp)
  target_include_directories(raw_root_fidelity_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(raw_root_fidelity_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(raw_root_fidelity_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME raw_root_fidelity_regression
           COMMAND raw_root_fidelity_tests)

  add_executable(waveform_dsp_tests
      tests/waveform_dsp_tests.cpp
      src/WaveformDsp.cpp)
  target_include_directories(waveform_dsp_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(waveform_dsp_tests PRIVATE
      -Wall -Wextra -Werror)
  set_target_properties(waveform_dsp_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME waveform_dsp_regression COMMAND waveform_dsp_tests)

  if(BUILD_DAQ_APPLICATIONS)
    add_test(NAME raw_salvage_hardware_free_integration
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/raw_salvage_integration_test.py
                $<TARGET_FILE:raw_salvage_dt5730>)
  endif()

  add_test(NAME gui_runtime_path_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_runtime_paths.py)
  add_test(NAME gui_trigger_threshold_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_trigger_settings.py)
  add_test(NAME gui_process_output_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_process_output.py)
  add_test(NAME gui_root_validation_output_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v
              tests/test_root_validation_output.py)
  add_test(NAME gui_database_manager_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v
              tests/test_database_manager.py)
  set_tests_properties(
      gui_runtime_path_regression gui_trigger_threshold_regression
      gui_process_output_regression gui_root_validation_output_regression
      gui_database_manager_regression
      PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # The operator/lifecycle suites instantiate Qt widgets and, through
  # MainWindow, the complete monitoring stack.  Prefer the same project-local
  # venv used by bin/daq_gui, but do not turn a hardware-free/test-only CMake
  # configure into a failure when optional GUI dependencies are absent.
  set(_CPNR_GUI_TEST_PYTHON_CANDIDATES)
  if(EXISTS "${CMAKE_SOURCE_DIR}/build/venv/bin/python")
    list(APPEND _CPNR_GUI_TEST_PYTHON_CANDIDATES
         "${CMAKE_SOURCE_DIR}/build/venv/bin/python")
  endif()
  if(EXISTS "${CMAKE_BINARY_DIR}/venv/bin/python")
    list(APPEND _CPNR_GUI_TEST_PYTHON_CANDIDATES
         "${CMAKE_BINARY_DIR}/venv/bin/python")
  endif()
  list(APPEND _CPNR_GUI_TEST_PYTHON_CANDIDATES "${Python3_EXECUTABLE}")
  list(REMOVE_DUPLICATES _CPNR_GUI_TEST_PYTHON_CANDIDATES)

  unset(CPNR_GUI_TEST_PYTHON)
  foreach(_CPNR_PYTHON_CANDIDATE IN LISTS _CPNR_GUI_TEST_PYTHON_CANDIDATES)
    execute_process(
        COMMAND "${_CPNR_PYTHON_CANDIDATE}" -c
                "import PyQt6, numpy, pyqtgraph, zmq"
        RESULT_VARIABLE _CPNR_GUI_IMPORT_RESULT
        OUTPUT_QUIET ERROR_QUIET)
    if(_CPNR_GUI_IMPORT_RESULT EQUAL 0)
      set(CPNR_GUI_TEST_PYTHON "${_CPNR_PYTHON_CANDIDATE}")
      break()
    endif()
  endforeach()

  if(CPNR_GUI_TEST_PYTHON)
    add_test(NAME gui_operator_safety_regression
        COMMAND ${CPNR_GUI_TEST_PYTHON} -m unittest -v
                tests/test_gui_operator_safety.py)
    add_test(NAME gui_process_lifecycle_regression
        COMMAND ${CPNR_GUI_TEST_PYTHON} -m unittest -v
                tests/test_process_manager_lifecycle.py)
    add_test(NAME gui_root_validation_lifecycle_regression
        COMMAND ${CPNR_GUI_TEST_PYTHON} -m unittest -v
                tests/test_root_validation_tab_lifecycle.py)
    add_test(NAME gui_monitor_safety_regression
        COMMAND ${CPNR_GUI_TEST_PYTHON} -m unittest -v
                tests/test_monitor_safety.py)
    set_tests_properties(
        gui_operator_safety_regression gui_process_lifecycle_regression
        gui_root_validation_lifecycle_regression gui_monitor_safety_regression
        PROPERTIES
          WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
          ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
          TIMEOUT 60)
    message(STATUS
        "GUI operator/lifecycle tests: ${CPNR_GUI_TEST_PYTHON} (offscreen)")
  else()
    message(STATUS
        "GUI operator/lifecycle tests not registered: install requirements.txt "
        "in build/venv or configure with a Python containing the GUI stack")
  endif()
  unset(_CPNR_GUI_IMPORT_RESULT)
  unset(_CPNR_PYTHON_CANDIDATE)
  unset(_CPNR_GUI_TEST_PYTHON_CANDIDATES)
endif()

if(BUILD_DAQ_APPLICATIONS)
  execute_process(
      COMMAND git rev-parse --verify HEAD
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE CPNR_GIT_COMMIT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  if(NOT CPNR_GIT_COMMIT)
    set(CPNR_GIT_COMMIT "unknown")
  endif()
  execute_process(
      COMMAND git status --porcelain --untracked-files=all --
              CMakeLists.txt README.md requirements.txt
              src include gui config tests
              ":(exclude,glob)gui/**/__pycache__/**"
              ":(exclude,glob)gui/**/*.pyc"
              ":(exclude,glob)tests/**/__pycache__/**"
              ":(exclude,glob)tests/**/*.pyc"
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE CPNR_GIT_STATUS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE CPNR_GIT_STATUS_RESULT
      ERROR_QUIET)
  if(CPNR_GIT_STATUS_RESULT EQUAL 0)
    if(CPNR_GIT_STATUS)
      string(APPEND CPNR_GIT_COMMIT "+dirty")
    else()
      string(APPEND CPNR_GIT_COMMIT "+clean")
    endif()
  else()
    string(APPEND CPNR_GIT_COMMIT "+dirty-state-unknown")
  endif()
  message(STATUS "Runtime git identity: ${CPNR_GIT_COMMIT}")
  string(TIMESTAMP CPNR_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

  # [배포 자동화] 실행 파일 출력 디렉토리를 프로젝트 루트의 bin/ 폴더로 지정
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

  # [배포 자동화] 프로젝트 루트에 필수 디렉토리 자동 생성
  file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/lib
                      ${CMAKE_SOURCE_DIR}/data
                      ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config)

  # [배포 자동화] 설정 파일(.conf)들을 bin/config/ 경로로 자동 복사
  file(COPY ${CMAKE_SOURCE_DIR}/config/
       DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config/
       FILES_MATCHING PATTERN "*.conf")

  # [Tier 1] Frontend
  add_executable(raw_salvage_dt5730
      src/raw_salvage_dt5730.cpp
      src/DAQConfig.cpp
      src/Sha256.cpp)
  target_include_directories(raw_salvage_dt5730 PRIVATE include)
  target_compile_options(raw_salvage_dt5730 PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(raw_salvage_dt5730 PRIVATE
      CPNR_GIT_COMMIT="${CPNR_GIT_COMMIT}"
      CPNR_BUILD_TIMESTAMP="${CPNR_BUILD_TIMESTAMP}")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(raw_salvage_dt5730 PRIVATE stdc++fs)
  endif()

  if(BUILD_DAQ_FRONTEND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
    find_package(Threads REQUIRED)

    set(CAEN_ROOT "" CACHE PATH
        "CAEN SDK prefix containing include/ and lib/ (or lib64/)")
    set(_CAEN_HINTS ${CAEN_ROOT} $ENV{CAEN_ROOT})
    set(_CAEN_SEARCH_MODE)
    if(CAEN_ROOT OR NOT "$ENV{CAEN_ROOT}" STREQUAL "")
      # An explicit SDK prefix is authoritative. Do not silently combine its
      # header with ABI-incompatible CAEN libraries from a system location.
      set(_CAEN_SEARCH_MODE NO_DEFAULT_PATH)
    endif()
    find_path(CAENDIGITIZER_INCLUDE_DIR
        NAMES CAENDigitizer.h
        HINTS ${_CAEN_HINTS}
        PATH_SUFFIXES include
        ${_CAEN_SEARCH_MODE})
    find_library(CAENDIGITIZER_LIBRARY
        NAMES CAENDigitizer
        HINTS ${_CAEN_HINTS}
        PATH_SUFFIXES lib lib64
        ${_CAEN_SEARCH_MODE})
    find_library(CAENCOMM_LIBRARY
        NAMES CAENComm
        HINTS ${_CAEN_HINTS}
        PATH_SUFFIXES lib lib64
        ${_CAEN_SEARCH_MODE})
    find_library(CAENVME_LIBRARY
        NAMES CAENVME
        HINTS ${_CAEN_HINTS}
        PATH_SUFFIXES lib lib64
        ${_CAEN_SEARCH_MODE})
    find_library(LIBUSB_LIBRARY
        NAMES usb-1.0
        HINTS ${_CAEN_HINTS}
        PATH_SUFFIXES lib lib64)
    if(NOT CAENDIGITIZER_INCLUDE_DIR OR
       NOT CAENDIGITIZER_LIBRARY OR
       NOT CAENCOMM_LIBRARY OR
       NOT CAENVME_LIBRARY OR
       NOT LIBUSB_LIBRARY)
      message(FATAL_ERROR
          "CAEN SDK not found. Install CAENVMELib, CAENComm, and "
          "CAENDigitizer plus libusb-1.0, or configure with "
          "-DCAEN_ROOT=<prefix>. "
          "Found header='${CAENDIGITIZER_INCLUDE_DIR}', "
          "Digitizer='${CAENDIGITIZER_LIBRARY}', "
          "Comm='${CAENCOMM_LIBRARY}', VME='${CAENVME_LIBRARY}', "
          "libusb='${LIBUSB_LIBRARY}'.")
    endif()

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${CAENDIGITIZER_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES
        ${CAENDIGITIZER_LIBRARY} ${CAENCOMM_LIBRARY} ${CAENVME_LIBRARY}
        ${LIBUSB_LIBRARY})
    unset(CAEN_HAS_OPEN_DIGITIZER2 CACHE)
    check_cxx_source_compiles(
        "#include <CAENDigitizer.h>\nint main() { int handle = -1; return CAEN_DGTZ_OpenDigitizer2(CAEN_DGTZ_USB, nullptr, 0, 0, &handle); }"
        CAEN_HAS_OPEN_DIGITIZER2)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(NOT CAEN_HAS_OPEN_DIGITIZER2)
      message(FATAL_ERROR
          "The selected CAENDigitizer SDK does not provide "
          "CAEN_DGTZ_OpenDigitizer2. Install CAENDigitizer >= 2.17.0.")
    endif()

    add_executable(frontend_dt5730
        src/frontend_dt5730.cpp
        src/DAQManager.cpp
        src/DAQConfig.cpp
        src/TriggerCalibration.cpp
        src/Sha256.cpp)
    target_include_directories(frontend_dt5730 PRIVATE
        include ${CAENDIGITIZER_INCLUDE_DIR})
    target_compile_definitions(frontend_dt5730 PRIVATE
        CPNR_GIT_COMMIT="${CPNR_GIT_COMMIT}"
        CPNR_BUILD_TIMESTAMP="${CPNR_BUILD_TIMESTAMP}")
    # Ubuntu links with --as-needed by default. Keep CAEN's transitive runtime
    # stack as direct ELF dependencies so the executable RUNPATH also resolves
    # CAENDigitizer -> CAENComm -> CAENVME -> libusb without LD_LIBRARY_PATH.
    if(UNIX AND NOT APPLE)
      target_link_options(frontend_dt5730 PRIVATE "LINKER:--no-as-needed")
    endif()
    target_link_libraries(frontend_dt5730 PRIVATE
        PkgConfig::ZMQ
        ${CAENDIGITIZER_LIBRARY}
        ${CAENCOMM_LIBRARY}
        ${CAENVME_LIBRARY}
        ${LIBUSB_LIBRARY}
        Threads::Threads)
    message(STATUS "CAEN SDK include: ${CAENDIGITIZER_INCLUDE_DIR}")
    message(STATUS "CAENDigitizer library: ${CAENDIGITIZER_LIBRARY}")
    message(STATUS "CAENComm library: ${CAENCOMM_LIBRARY}")
    message(STATUS "CAENVME library: ${CAENVME_LIBRARY}")
    message(STATUS "libusb library: ${LIBUSB_LIBRARY}")
  endif()

  # [Tier 2] Production and read-only ROOT validation
  if(BUILD_PRODUCTION_CONVERTER OR BUILD_ROOT_VALIDATOR)
    find_package(ROOT QUIET COMPONENTS Core Tree Hist RIO Rint Gpad Graf Gui)
    if(ROOT_FOUND)
      include(${ROOT_USE_FILE})
      if(BUILD_PRODUCTION_CONVERTER)
        add_executable(production_dt5730
            src/production_dt5730.cpp
            src/DAQConfig.cpp
            src/WaveformDsp.cpp
            src/Sha256.cpp)
        target_include_directories(production_dt5730 PRIVATE include)
        target_compile_definitions(production_dt5730 PRIVATE
            CPNR_GIT_COMMIT="${CPNR_GIT_COMMIT}"
            CPNR_BUILD_TIMESTAMP="${CPNR_BUILD_TIMESTAMP}")
        target_link_libraries(production_dt5730 ${ROOT_LIBRARIES})
        if(BUILD_TESTING)
          add_test(NAME production_provenance_integration
              COMMAND ${Python3_EXECUTABLE}
                      ${CMAKE_SOURCE_DIR}/tests/production_integration_test.py
                      $<TARGET_FILE:production_dt5730>
                      ${CMAKE_SOURCE_DIR}/config/dt5730s_ls_coin.conf
                      ${ROOT_root_CMD}
                      ${ROOT_rootls_CMD})
          set_tests_properties(production_provenance_integration PROPERTIES
              WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
              RESOURCE_LOCK production_converter_fixture)
        endif()
      endif()

      if(BUILD_ROOT_VALIDATOR)
        add_executable(root_validate_dt5730
            src/root_validate_dt5730.cpp
            src/RootValidator.cpp
            src/RawRootFidelity.cpp
            src/WaveformDsp.cpp
            src/DAQConfig.cpp
            src/Sha256.cpp)
        target_include_directories(root_validate_dt5730 PRIVATE include)
        target_compile_options(root_validate_dt5730 PRIVATE
            -Wall -Wextra -Werror)
        target_compile_definitions(root_validate_dt5730 PRIVATE
            CPNR_GIT_COMMIT="${CPNR_GIT_COMMIT}"
            CPNR_BUILD_TIMESTAMP="${CPNR_BUILD_TIMESTAMP}")
        target_link_libraries(root_validate_dt5730 ${ROOT_LIBRARIES})
        if(BUILD_TESTING AND BUILD_PRODUCTION_CONVERTER)
          add_test(NAME root_validation_integration
              COMMAND ${Python3_EXECUTABLE}
                      ${CMAKE_SOURCE_DIR}/tests/root_validation_integration_test.py
                      $<TARGET_FILE:root_validate_dt5730>
                      $<TARGET_FILE:production_dt5730>
                      ${CMAKE_SOURCE_DIR}/config/dt5730s_ls_coin.conf
                      ${ROOT_root_CMD})
          set_tests_properties(root_validation_integration PROPERTIES
              WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
              RESOURCE_LOCK production_converter_fixture)
        endif()
      endif()
    else()
      message(FATAL_ERROR
          "ROOT framework was requested but not found. Install ROOT or "
          "explicitly configure -DBUILD_PRODUCTION_CONVERTER=OFF "
          "-DBUILD_ROOT_VALIDATOR=OFF. A successful default build must not "
          "silently omit production/validation executables and tests.")
    endif()
  endif()

  # ==============================================================================
  # [Requirement] GUI Deployment Automation
  # ==============================================================================
  # 1. GUI/config deployment is an explicit always-built target. Remove only
  #    the generated GUI destination first so deleted/renamed source modules
  #    cannot survive there; keep bin/config because it may contain run-time
  #    configuration edited by the operator.
  add_custom_target(deploy_runtime ALL
      COMMAND ${CMAKE_COMMAND} -E rm -rf
      ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gui
      COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/gui ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gui
      COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/config ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config
      COMMENT "Deploying GUI and configuration files to bin/..."
  )

  # 2. 메인 GUI 실행을 위한 래퍼 셸 스크립트 자동 생성. 프로젝트의
  # build/venv를 우선 사용하되, 시스템 설치를 사용하는 기존 환경도 유지합니다.
  # 작업 디렉토리는 항상 bin/으로 고정합니다.
  file(WRITE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui [=[#!/bin/bash
set -euo pipefail
script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
python_bin="$project_root/build/venv/bin/python"
if [[ ! -x "$python_bin" ]]; then
  python_bin="$(command -v python3)"
fi
# Prefer project-local native libraries. Besides the CAEN stack this also lets
# a non-root installation provide Qt's small XCB runtime dependencies.
for local_lib in \
    "$project_root/build/local/lib" \
    "$project_root"/build/local/usr/lib/*-linux-gnu; do
  if [[ -d "$local_lib" ]]; then
    export LD_LIBRARY_PATH="$local_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
done
cd "$script_dir"
exec "$python_bin" gui/main.py "$@"
]=])
  execute_process(COMMAND chmod +x ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui)
endif()
