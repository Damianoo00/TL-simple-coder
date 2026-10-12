cmake_minimum_required(VERSION 3.20)
project(async_nodes LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)

add_library(graph_core STATIC
    core/Node.cpp
    nodes/InputNode.cpp
    nodes/OutputNode.cpp
    api/JsonGraphParser.cpp
)
target_compile_features(graph_core PUBLIC cxx_std_20)
set_target_properties(graph_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(graph_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(graph_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)

add_executable(graph_worker api/GraphWorker.cpp)
target_link_libraries(graph_worker PRIVATE graph_core)

add_executable(supervisor api/Supervisor.cpp)
target_link_libraries(supervisor PRIVATE graph_core Crow::Crow)

foreach(target graph_core graph_worker supervisor)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

add_custom_target(run
    COMMAND $<TARGET_FILE:supervisor> $<TARGET_FILE:graph_worker>
    DEPENDS supervisor graph_worker
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME integration
        COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "GRAPH_WORKER=$<TARGET_FILE:graph_worker>"
            "SUPERVISOR=$<TARGET_FILE:supervisor>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py"
    )
endif()
