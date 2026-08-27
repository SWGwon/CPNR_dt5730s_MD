cmake_minimum_required(VERSION 3.17)
project(CAEN_DT5730_3TIER_DAQ CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra -pthread -fPIC")

include(CTest)
option(BUILD_DAQ_APPLICATIONS "Build hardware DAQ applications and deploy the GUI" ON)

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
endif()

if(BUILD_DAQ_APPLICATIONS)
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

  # 패키지 탐색 (ZeroMQ)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ZMQ REQUIRED libzmq)

  # ROOT Framework 탐색
  find_package(ROOT QUIET COMPONENTS Core Tree Hist RIO Rint Gpad Graf Gui)

  # [Tier 1] Frontend
  add_executable(frontend_dt5730 src/frontend_dt5730.cpp src/DAQManager.cpp src/DAQConfig.cpp)
  target_include_directories(frontend_dt5730 PRIVATE include ${ZMQ_INCLUDE_DIRS})
  target_link_libraries(frontend_dt5730 ${ZMQ_LIBRARIES} CAENDigitizer CAENComm CAENVME pthread)

  # [Tier 2] Production
  if(ROOT_FOUND)
    include(${ROOT_USE_FILE})
    add_executable(production_dt5730 src/production_dt5730.cpp)
    target_include_directories(production_dt5730 PRIVATE include)
    target_link_libraries(production_dt5730 ${ROOT_LIBRARIES})
  else()
    message(WARNING "ROOT framework not found. Skipping 'production_dt5730' build.")
  endif()

  # ==============================================================================
  # [Requirement] GUI Deployment Automation
  # ==============================================================================
  # 1. 빌드 완료 후 gui 폴더의 모든 파이썬 스크립트를 bin/gui/ 경로로 자동 복사
  add_custom_command(TARGET frontend_dt5730 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/gui ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gui
      COMMENT "Copying GUI files to binary directory..."
  )

  # 2. 메인 GUI 실행을 위한 래퍼 셸 스크립트 자동 생성
  # 작업 디렉토리를 항상 'bin/'으로 고정하여 상대 경로(config, data) 탐색 시 혼선 방지
  file(WRITE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui "#!/bin/bash\ncd \"\$(dirname \"\$0\")\"\npython3 gui/main.py \"\$@\"\n")
  execute_process(COMMAND chmod +x ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/daq_gui)
endif()
