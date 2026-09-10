# ==============================================================================
# VTX SDK — module orchestrator
# ==============================================================================

add_subdirectory(vtx_common)

if(VTX_BUILD_WRITER)
    add_subdirectory(vtx_writer)
endif()

if(VTX_BUILD_READER)
    add_subdirectory(vtx_reader)
endif()

if(VTX_BUILD_DIFFER)
    add_subdirectory(vtx_differ)
endif()

# Tools that modify an existing replay (repair, cut, entity filter): they read
# through vtx_reader and re-serialize through vtx_writer's policies, so it needs both.
if(VTX_BUILD_TRANSFORM AND VTX_BUILD_READER AND VTX_BUILD_WRITER)
    add_subdirectory(vtx_transform)
elseif(VTX_BUILD_TRANSFORM)
    message(STATUS "VTX: vtx_transform skipped (requires VTX_BUILD_READER and VTX_BUILD_WRITER).")
endif()
