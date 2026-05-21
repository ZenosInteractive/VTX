project(vtx_writer)

# ==============================================================================
# 1. Source files and headers
# ==============================================================================
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/writer/*.h"
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/writer/*.hpp"
)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# ==============================================================================
# 2. Target and deps
# ==============================================================================
# Library type (STATIC/SHARED) follows BUILD_SHARED_LIBS (VTX_BUILD_SHARED).
add_library(vtx_writer ${SOURCES} ${HEADERS})

target_include_directories(vtx_writer
    PRIVATE
        "${VTX_COMMON_GENERATED_DIR}"
)

target_link_libraries(vtx_writer
    PUBLIC
        vtx_common
        # network_sink.h is a public header-only template: any consumer that
        # instantiates ChunkedNetworkSink compiles Winsock calls into its own
        # objects, so ws2_32 must propagate through the link interface.
        $<$<PLATFORM_ID:Windows>:ws2_32>
    PRIVATE
        $<BUILD_INTERFACE:VTX::deps::protobuf>
        $<BUILD_INTERFACE:VTX::deps::flatbuffers>
        $<BUILD_INTERFACE:VTX::deps::zstd>
        # IXWebSocket powers the WebSocket data source; it stays PRIVATE --
        # websocket_client.cpp (PIMPL) is the only TU that touches it, so the
        # dependency never reaches the public SDK headers.
        $<BUILD_INTERFACE:VTX::deps::ixwebsocket>
)

# See note in vtx_reader/CMakeLists.txt -- Windows shared DLLs duplicate the
# generated protobuf/flatbuffers objects into every module that references them.
if(BUILD_SHARED_LIBS AND WIN32)
    target_link_libraries(vtx_writer PRIVATE $<BUILD_INTERFACE:vtx_generated_code>)
endif()

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_writer EXPORT VTXTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
