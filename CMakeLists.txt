cmake_minimum_required(VERSION 3.17)
project(CAEN_DT5730_3TIER_DAQ CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra -pthread -fPIC")

# Re-run configure before a build whenever provenance-relevant source/config or
# the current main ref/index changes. This keeps embedded commit/dirty stamps
# from silently lagging behind a plain `cmake --build` invocation.
file(GLOB_RECURSE CPNR_PROVENANCE_INPUTS LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/src/*" "${CMAKE_SOURCE_DIR}/include/*"
    "${CMAKE_SOURCE_DIR}/gui/*" "${CMAKE_SOURCE_DIR}/config/*"
    "${CMAKE_SOURCE_DIR}/tests/*" "${CMAKE_SOURCE_DIR}/README.md"
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
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

if(BUILD_TESTING)
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
  add_test(NAME frontend_rejects_zero_run_number
      COMMAND frontend_hardware_free_compile -r 0)
  set_tests_properties(frontend_rejects_zero_run_number
      PROPERTIES WILL_FAIL TRUE)

  add_executable(sha256_tests tests/sha256_tests.cpp src/Sha256.cpp)
  target_include_directories(sha256_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(sha256_tests PRIVATE -Wall -Wextra -Werror)
  set_target_properties(sha256_tests PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_test(NAME sha256_regression COMMAND sha256_tests)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME gui_runtime_path_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_runtime_paths.py)
  add_test(NAME gui_trigger_threshold_regression
      COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_trigger_settings.py)
  set_tests_properties(
      gui_runtime_path_regression gui_trigger_threshold_regression
      PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
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
              CMakeLists.txt README.md src include gui config tests
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
  if(BUILD_DAQ_FRONTEND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZMQ REQUIRED libzmq)
    add_executable(frontend_dt5730
        src/frontend_dt5730.cpp
        src/DAQManager.cpp
        src/DAQConfig.cpp
        src/TriggerCalibration.cpp
        src/Sha256.cpp)
    target_include_directories(frontend_dt5730 PRIVATE include ${ZMQ_INCLUDE_DIRS})
    target_compile_definitions(frontend_dt5730 PRIVATE
        CPNR_GIT_COMMIT="${CPNR_GIT_COMMIT}"
        CPNR_BUILD_TIMESTAMP="${CPNR_BUILD_TIMESTAMP}")
    target_link_libraries(frontend_dt5730 ${ZMQ_LIBRARIES} CAENDigitizer CAENComm CAENVME pthread)
  endif()

  # [Tier 2] Production
  if(BUILD_PRODUCTION_CONVERTER)
    find_package(ROOT QUIET COMPONENTS Core Tree Hist RIO Rint Gpad Graf Gui)
    if(ROOT_FOUND)
      include(${ROOT_USE_FILE})
      add_executable(production_dt5730
          src/production_dt5730.cpp
          src/DAQConfig.cpp
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
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
      endif()
    else()
      message(WARNING "ROOT framework not found. Skipping 'production_dt5730' build.")
    endif()
  endif()

  # ==============================================================================
  # [Requirement] GUI Deployment Automation
  # ==============================================================================
  # 1. GUI/config deployment is an explicit always-built target, so a stale
  #    bin/gui cannot survive a production-only or frontend-disabled rebuild.
  add_custom_target(deploy_runtime ALL
      COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/gui ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gui
      COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/config ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config
      COMMENT "Deploying GUI and configuration files to bin/..."
  )

  # 2. 메인 GUI 실행을 위한 래퍼 셸 스크립트 자동 생성
  # 작업 디렉토리를 항상 'bin/'으로 고정하여 상대 경로(config, data) 탐색 시 혼선 방지
  file(WRITE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui "#!/bin/bash\ncd \"\$(dirname \"\$0\")\"\npython3 gui/main.py \"\$@\"\n")
  execute_process(COMMAND chmod +x ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui)
endif()
