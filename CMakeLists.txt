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
    nodes/BrowserRenderer.cpp
    nodes/VideoOverlayNode.cpp
    nodes/AudioResamplerNode.cpp
    nodes/MuxerNode.cpp
    api/JsonGraphParser.cpp
)
target_compile_features(graph_core PUBLIC cxx_std_20)
set_target_properties(graph_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(graph_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(graph_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json ffmpeg::avformat ffmpeg::avcodec ffmpeg::avutil ffmpeg::swscale ffmpeg::swresample)
set(OVERLAY_ELECTRON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/overlay-renderer/node_modules/electron/dist/electron" CACHE FILEPATH "Electron executable for browser overlays")
target_compile_definitions(graph_core PRIVATE
    OVERLAY_ELECTRON_PATH="${OVERLAY_ELECTRON_PATH}"
    OVERLAY_SCRIPT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/overlay-renderer/main.cjs")

add_executable(graph_worker api/GraphWorker.cpp)
target_link_libraries(graph_worker PRIVATE graph_core)

add_executable(supervisor api/Supervisor.cpp)
target_link_libraries(supervisor PRIVATE graph_core Crow::Crow)
target_compile_definitions(supervisor PRIVATE
    ASYNC_NODES_FRONTEND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")

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
    foreach(unit_test json_graph_parser media_validation output_protocol input_protocol)
        add_executable(test_${unit_test} tests/test_${unit_test}.cpp)
        target_link_libraries(test_${unit_test} PRIVATE graph_core)
        add_test(NAME ${unit_test} COMMAND test_${unit_test})
        set_tests_properties(${unit_test} PROPERTIES TIMEOUT 20)
    endforeach()
    add_executable(test_thread_safe_queue tests/test_thread_safe_queue.cpp)
    target_include_directories(test_thread_safe_queue PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(test_thread_safe_queue PRIVATE cxx_std_20)
    target_link_libraries(test_thread_safe_queue PRIVATE Threads::Threads)
    add_test(NAME thread_safe_queue COMMAND test_thread_safe_queue)
    add_executable(test_node_lifecycle tests/test_node_lifecycle.cpp)
    target_link_libraries(test_node_lifecycle PRIVATE graph_core)
    add_test(NAME node_lifecycle COMMAND test_node_lifecycle)
    set_tests_properties(thread_safe_queue node_lifecycle PROPERTIES TIMEOUT 10)
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
    add_executable(test_overlay_node tests/test_overlay_node.cpp)
    target_link_libraries(test_overlay_node PRIVATE graph_core)
    target_compile_definitions(test_overlay_node PRIVATE
        TEST_PYTHON="${Python3_EXECUTABLE}"
        FAKE_OVERLAY_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_overlay_renderer.py")
    add_test(NAME overlay_node COMMAND test_overlay_node)
    set_tests_properties(overlay_node PROPERTIES TIMEOUT 10)
    set_tests_properties(json_graph_parser media_validation output_protocol input_protocol
        thread_safe_queue node_lifecycle frame_conversion codec_nodes demux_mux
        typed_nodes overlay_node PROPERTIES LABELS "unit")
    add_test(NAME integration
        COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONDONTWRITEBYTECODE=1"
            "GRAPH_WORKER=$<TARGET_FILE:graph_worker>"
            "SUPERVISOR=$<TARGET_FILE:supervisor>"
            "MEDIA_PROTOCOL_TEST=$<TARGET_FILE:test_media_protocol>"
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py"
    )
    set_tests_properties(integration PROPERTIES LABELS "integration" TIMEOUT 120)
endif()
