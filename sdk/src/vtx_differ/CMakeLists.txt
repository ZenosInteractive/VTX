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
add_library(vtx_differ STATIC ${SOURCES} ${HEADERS})

target_include_directories(vtx_differ
    PUBLIC
    PRIVATE
        "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include"
        "${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated"
)

target_link_libraries(vtx_differ PUBLIC vtx_common)

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_differ EXPORT VTXTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
