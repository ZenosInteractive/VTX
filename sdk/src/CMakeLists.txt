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
