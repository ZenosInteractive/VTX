project(vtx_differ)

# ==============================================================================
# 1. Source files and headers
# ==============================================================================
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/differ/*.h"
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/differ/*.hpp"
)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# ==============================================================================
# 2. Target and deps
# ==============================================================================
# Library type (STATIC/SHARED) follows BUILD_SHARED_LIBS (VTX_BUILD_SHARED).
add_library(vtx_differ ${SOURCES} ${HEADERS})

target_include_directories(vtx_differ
    PRIVATE
        "${VTX_COMMON_GENERATED_DIR}"
)

target_link_libraries(vtx_differ
    PUBLIC
        vtx_common
    PRIVATE
        $<BUILD_INTERFACE:VTX::deps::protobuf>
        $<BUILD_INTERFACE:VTX::deps::flatbuffers>
        $<BUILD_INTERFACE:VTX::deps::zstd>
)

# See note in vtx_reader/CMakeLists.txt -- Windows shared DLLs duplicate the
# generated protobuf/flatbuffers objects into every module that references them.
if(BUILD_SHARED_LIBS AND WIN32)
    target_link_libraries(vtx_differ PRIVATE $<BUILD_INTERFACE:vtx_generated_code>)
endif()

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_differ EXPORT VTXTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
