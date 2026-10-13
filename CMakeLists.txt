cmake_minimum_required(VERSION 3.20)
project(async_nodes LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(ffmpeg CONFIG REQUIRED)

add_library(graph_core STATIC
    core/Node.cpp
    nodes/InputNode.cpp
    nodes/OutputNode.cpp
    nodes/DemuxNode.cpp
    nodes/DecoderNode.cpp
    nodes/EncoderNode.cpp
    nodes/FrameTransformNode.cpp
    nodes/VideoScalerNode.cpp
    nodes/AudioResamplerNode.cpp
    nodes/MuxerNode.cpp
    api/JsonGraphParser.cpp
)
target_compile_features(graph_core PUBLIC cxx_std_20)
set_target_properties(graph_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(graph_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(graph_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json ffmpeg::avformat ffmpeg::avcodec ffmpeg::avutil ffmpeg::swscale ffmpeg::swresample)

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
    add_executable(test_frame_conversion tests/test_frame_conversion.cpp)
    target_link_libraries(test_frame_conversion PRIVATE graph_core)
    add_test(NAME frame_conversion COMMAND test_frame_conversion)
    set_tests_properties(frame_conversion PROPERTIES TIMEOUT 20)
    add_executable(test_codec_nodes tests/test_codec_nodes.cpp)
    target_link_libraries(test_codec_nodes PRIVATE graph_core)
    add_test(NAME codec_nodes COMMAND test_codec_nodes)
    set_tests_properties(codec_nodes PROPERTIES TIMEOUT 20)
    add_executable(test_demux_mux tests/test_demux_mux.cpp)
    target_link_libraries(test_demux_mux PRIVATE graph_core)
    add_test(NAME demux_mux COMMAND test_demux_mux)
    set_tests_properties(demux_mux PROPERTIES TIMEOUT 10)
    add_executable(test_media_protocol tests/test_media_protocol.cpp)
    target_link_libraries(test_media_protocol PRIVATE graph_core)
    add_executable(test_typed_nodes tests/test_typed_nodes.cpp)
    target_link_libraries(test_typed_nodes PRIVATE graph_core)
    add_test(NAME typed_nodes COMMAND test_typed_nodes)
    set_tests_properties(typed_nodes PROPERTIES TIMEOUT 10)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME integration
        COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "GRAPH_WORKER=$<TARGET_FILE:graph_worker>"
            "SUPERVISOR=$<TARGET_FILE:supervisor>"
            "MEDIA_PROTOCOL_TEST=$<TARGET_FILE:test_media_protocol>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py"
    )
endif()
