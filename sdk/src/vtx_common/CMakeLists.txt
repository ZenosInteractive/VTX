project(vtx_common)

# ==============================================================================
# 1. Dependencies (Thirdparty)
# ==============================================================================
set(PROTO_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/protobuf")
set(PROTOC_EXE "${PROTO_ROOT}/bin/protoc.exe")
set(PROTO_LIB_RELEASE "${PROTO_ROOT}/lib/libprotobuf.lib")
set(PROTO_LIB_DEBUG   "${PROTO_ROOT}/lib/libprotobufd.lib")

set(FLAT_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers")
set(FLATC_EXE "${FLAT_ROOT}/bin/flatc.exe")
set(FLAT_LIB_RELEASE "${FLAT_ROOT}/lib/flatbuffers.lib")
set(FLAT_LIB_DEBUG   "${FLAT_ROOT}/lib/flatbuffersd.lib")

set(ZSTD_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/zstd")
set(ZSTD_LIB_RELEASE "${ZSTD_ROOT}/lib/Release/zstd.lib")
set(ZSTD_LIB_DEBUG   "${ZSTD_ROOT}/lib/Debug/zstd.lib")
set(ZSTD_DLL_RELEASE "${ZSTD_ROOT}/bin/Release/zstd.dll")
set(ZSTD_DLL_DEBUG   "${ZSTD_ROOT}/bin/Debug/zstd.dll")

set(JSON_INC "${CMAKE_SOURCE_DIR}/thirdparty/jsonlohmann/include")
set(XXHASH_INC "${CMAKE_SOURCE_DIR}/thirdparty/xxhash")

# ==============================================================================
# 2. Schema generators (Protobuf & Flatbuffers)
# ==============================================================================
set(SCHEMA_DIR "${CMAKE_SOURCE_DIR}/sdk/src/schemas")
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_FILENAME "vtx_schema")
set(PROTO_FILE "${SCHEMA_DIR}/${PROTO_FILENAME}.proto")
set(PROTO_SRC "${GENERATED_DIR}/${PROTO_FILENAME}.pb.cc")
set(PROTO_HDR "${GENERATED_DIR}/${PROTO_FILENAME}.pb.h")

add_custom_command(
    OUTPUT ${PROTO_SRC} ${PROTO_HDR}
    COMMAND "${PROTOC_EXE}"
    ARGS --cpp_out "${GENERATED_DIR}" -I . "${PROTO_FILENAME}.proto"
    WORKING_DIRECTORY "${SCHEMA_DIR}"
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating Protobuf C++..."
)

set(FBS_FILENAME "vtx_schema")
set(FBS_FILE "${SCHEMA_DIR}/${FBS_FILENAME}.fbs")
set(FBS_HDR "${GENERATED_DIR}/${FBS_FILENAME}_generated.h")

add_custom_command(
    OUTPUT ${FBS_HDR} "${GENERATED_DIR}/vtx_schema_bfbs_generated.h"
    COMMAND "${FLATC_EXE}"
    ARGS --cpp --gen-object-api -b --schema --bfbs-gen-embed -o "${GENERATED_DIR}" "${FBS_FILENAME}.fbs"
    WORKING_DIRECTORY "${SCHEMA_DIR}"
    DEPENDS ${FBS_FILE}
    COMMENT "Generating FlatBuffers C++..."
)

# ==============================================================================
# 3. Source files
# ==============================================================================
file(GLOB_RECURSE EXISTING_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common/*.h"
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common/*.hpp"
)

file(GLOB_RECURSE EXISTING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

# ==============================================================================
# 4. Target (vtx_common)
# ==============================================================================
add_library(vtx_common STATIC
    ${PROTO_SRC} ${PROTO_HDR}
    ${FBS_HDR}
    ${EXISTING_HEADERS}
    ${EXISTING_SOURCES}
)

file(GLOB_RECURSE IDE_SCHEMA_FILES
    "${SCHEMA_DIR}/*.fbs"
    "${SCHEMA_DIR}/*.proto"
)
source_group("Schemas" FILES ${IDE_SCHEMA_FILES})
target_sources(vtx_common PRIVATE ${IDE_SCHEMA_FILES})

target_link_libraries(vtx_common PUBLIC
    $<$<CONFIG:Debug>:${PROTO_LIB_DEBUG}>
    $<$<CONFIG:Release>:${PROTO_LIB_RELEASE}>
    $<$<CONFIG:RelWithDebInfo>:${PROTO_LIB_RELEASE}>
    $<$<CONFIG:Debug>:${FLAT_LIB_DEBUG}>
    $<$<CONFIG:Release>:${FLAT_LIB_RELEASE}>
    $<$<CONFIG:RelWithDebInfo>:${FLAT_LIB_RELEASE}>
    $<$<CONFIG:Debug>:${ZSTD_LIB_DEBUG}>
    $<$<CONFIG:Release>:${ZSTD_LIB_RELEASE}>
    $<$<CONFIG:RelWithDebInfo>:${ZSTD_LIB_RELEASE}>
)

# ==============================================================================
# 5. Include directories
# ==============================================================================
target_include_directories(vtx_common
    PUBLIC
        $<BUILD_INTERFACE:${ZSTD_ROOT}/include>
        $<BUILD_INTERFACE:${JSON_INC}>
        $<BUILD_INTERFACE:${XXHASH_INC}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROTO_ROOT}/include"
        "${FLAT_ROOT}/include"
        "${GENERATED_DIR}"
)

target_compile_definitions(vtx_common PRIVATE PROTOBUF_STATIC_LIBRARY)

# ==============================================================================
# 6. VS filters
# ==============================================================================
source_group("Generated" FILES ${PROTO_SRC} ${PROTO_HDR} ${FBS_HDR})
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common" PREFIX "Header Files" FILES ${EXISTING_HEADERS})
source_group(TREE "${SCHEMA_DIR}" PREFIX "Schemas" FILES ${IDE_SCHEMA_FILES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${EXISTING_SOURCES})

add_custom_command(TARGET vtx_common POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${ZSTD_DLL_DEBUG}>
        $<$<CONFIG:Release>:${ZSTD_DLL_RELEASE}>
        $<$<CONFIG:RelWithDebInfo>:${ZSTD_DLL_RELEASE}>
        "$<TARGET_FILE_DIR:vtx_common>"
    COMMENT "Copying Zstd DLL to build dir..."
)

install(TARGETS vtx_common EXPORT VTXTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
