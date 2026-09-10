project(vtx_writer)

# ==============================================================================
# 1. Source files and headers
# ==============================================================================
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${VTX_SDK_SOURCE_DIR}/sdk/include/vtx/writer/*.h"
    "${VTX_SDK_SOURCE_DIR}/sdk/include/vtx/writer/*.hpp"
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

# Linux + shared build: IXWebSocket and mbedTLS are linked into this .so from static
# archives.  GNU ld would export their symbols with default visibility, so a consumer
# that links its own copy of the same archives (vtx_tests does, for its in-process
# WebSocket server) ends up with ONE interposed set of their globals -- constructed
# by both images and destroyed twice: `free(): double free detected` at exit.
# Hiding every archive keeps the .so self-contained with a private copy, which is
# what the Windows DLL already does (a DLL never exports the symbols of static
# libraries it links).  Nothing here is public API: consumers get zstd/flatbuffers
# through vtx_common's link interface, never through vtx_writer.
if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
    target_link_options(vtx_writer PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${VTX_SDK_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_writer EXPORT VTXTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
