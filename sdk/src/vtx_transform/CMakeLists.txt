project(vtx_transform)

# ==============================================================================
# vtx_transform -- tools that modify an existing .vtx (repair, cut, entity filter).
# ==============================================================================
# Sits ABOVE vtx_reader and vtx_writer: cut and filter read a finished .vtx
# through the reader and re-serialize through the writer's formatter policies;
# repair reconstructs a crashed one from its recovery journal (writer sink
# headers, no reader). All of them frame bytes with the shared
# vtx/common/vtx_replay_framing.h. Keeping them in their own module leaves reader
# and writer independent siblings on vtx_common -- the writer only produces new
# recordings. Requires VTX_BUILD_READER and VTX_BUILD_WRITER (enforced in sdk/src).
# ==============================================================================

# ------------------------------------------------------------------------------
# 1. Source files and headers
# ------------------------------------------------------------------------------
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${VTX_SDK_SOURCE_DIR}/sdk/include/vtx/transform/*.h"
    "${VTX_SDK_SOURCE_DIR}/sdk/include/vtx/transform/*.hpp"
)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# ------------------------------------------------------------------------------
# 2. Target and deps
# ------------------------------------------------------------------------------
# Library type (STATIC/SHARED) follows BUILD_SHARED_LIBS (VTX_BUILD_SHARED).
add_library(vtx_transform ${SOURCES} ${HEADERS})

target_include_directories(vtx_transform
    PRIVATE
        "${VTX_COMMON_GENERATED_DIR}"
)

# The public headers only need vtx_common types; reader and writer are
# implementation details (they still propagate as link dependencies).
target_link_libraries(vtx_transform
    PUBLIC
        vtx_common
    PRIVATE
        vtx_reader
        vtx_writer
        $<BUILD_INTERFACE:VTX::deps::protobuf>
        $<BUILD_INTERFACE:VTX::deps::flatbuffers>
        $<BUILD_INTERFACE:VTX::deps::zstd>
)

# See note in vtx_reader/CMakeLists.txt -- Windows shared DLLs duplicate the
# generated protobuf/flatbuffers objects into every module that references them.
if(BUILD_SHARED_LIBS AND WIN32)
    target_link_libraries(vtx_transform PRIVATE $<BUILD_INTERFACE:vtx_generated_code>)
endif()

# ------------------------------------------------------------------------------
# 3. VS filters
# ------------------------------------------------------------------------------
source_group(TREE "${VTX_SDK_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_transform EXPORT VTXTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
