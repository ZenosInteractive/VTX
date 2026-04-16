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
add_library(vtx_reader STATIC ${SOURCES} ${HEADERS})

target_include_directories(vtx_reader
    PUBLIC
    PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated"
        "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
        "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include"
)

target_compile_definitions(vtx_reader PRIVATE PROTOBUF_STATIC_LIBRARY)
target_link_libraries(vtx_reader
    PUBLIC
        vtx_common
    PRIVATE
        $<$<CONFIG:Debug>:${PROTO_LIB_DEBUG}>
        $<$<CONFIG:Release>:${PROTO_LIB_RELEASE}>
)

# ==============================================================================
# 3. VS filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include" PREFIX "Header Files" FILES ${HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})

install(TARGETS vtx_reader EXPORT VTXTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
