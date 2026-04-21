project(vtx_reader)

# ==============================================================================
# 1. Source files and headers
# ==============================================================================
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/reader/*.h"
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/reader/*.hpp"
)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# ==============================================================================
# 2. Target and deps
# ==============================================================================
#
# Third-party headers (protobuf, flatbuffers, xxhash, json) reach this target
# transitively through vtx_common, which links them PUBLIC.  We only need the
# private access to the generated sources directory.
# ==============================================================================
# Library type (STATIC/SHARED) follows BUILD_SHARED_LIBS (VTX_BUILD_SHARED).
add_library(vtx_reader ${SOURCES} ${HEADERS})

target_include_directories(vtx_reader
    PRIVATE
        "${VTX_COMMON_GENERATED_DIR}"
)

target_link_libraries(vtx_reader
    PUBLIC
        vtx_common
    PRIVATE
        $<BUILD_INTERFACE:VTX::deps::protobuf>
        $<BUILD_INTERFACE:VTX::deps::flatbuffers>
        $<BUILD_INTERFACE:VTX::deps::zstd>
)

# Windows + shared build: this DLL must embed its own copy of the generated
# protobuf/flatbuffers objects because Windows hides data symbols by default.
# On static builds or on Linux (default visibility) the final linker resolves
# those symbols through vtx_common without duplication.
if(BUILD_SHARED_LIBS AND WIN32)
    target_link_libraries(vtx_reader PRIVATE $<BUILD_INTERFACE:vtx_generated_code>)
endif()

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_reader EXPORT VTXTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
