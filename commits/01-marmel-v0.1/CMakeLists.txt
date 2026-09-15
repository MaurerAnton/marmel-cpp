cmake_minimum_required(VERSION 3.20)
project(marmel
  VERSION 0.1.0
  DESCRIPTION "Marmennill autonomous coding harness — C++ port of marmel v0.1 (commit b741f9e)"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(marmel_lib STATIC
  src/types.cpp
  src/config.cpp
  src/agent.cpp
  src/agents.cpp
  src/orchestrator.cpp
  src/harness.cpp
  src/harness_monitor.cpp
  src/llm.cpp
  src/mcp.cpp
  src/ui.cpp
  src/widget.cpp
)
target_include_directories(marmel_lib PUBLIC include)
target_compile_options(marmel_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(marmel_lib PUBLIC pthread)

# libcurl (HTTP backend for the LLM streaming client). Optional: without it
# the client reports a connectivity error (same bucket as offline backends).
find_package(CURL)
if(CURL_FOUND)
  target_compile_definitions(marmel_lib PUBLIC MARMEL_HAVE_CURL)
  target_link_libraries(marmel_lib PUBLIC CURL::libcurl)
else()
  message(STATUS "libcurl not found: building with stub HTTP transport")
endif()

# util (forkpty) on BSDs; on Linux it lives in libc.
if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|NetBSD|OpenBSD|DragonFly")
  target_link_libraries(marmel_lib PUBLIC util)
endif()

add_executable(marmel apps/marmel_main.cpp)
target_link_libraries(marmel PRIVATE marmel_lib)

enable_testing()
add_executable(marmel_tests tests/test_marmel.cpp)
target_link_libraries(marmel_tests PRIVATE marmel_lib)
target_compile_definitions(marmel_tests PRIVATE MARMEL_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/marmel.toml.example")
add_test(NAME marmel_tests COMMAND marmel_tests)
set_tests_properties(marmel_tests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  TIMEOUT 300)

