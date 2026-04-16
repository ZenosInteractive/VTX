#pragma once
#include "gui/gui_window.h"
#include "gui/gui_types.h"
#include "services/reader_chunk_state_service.h"

class InspectorSession;
namespace VTX {
class IVtxReaderFacade;
}

class ReaderInspectorWindow : public ImGuiWindow {
public:
    ReaderInspectorWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;
    void ApplyCacheWindowIfNeeded(VTX::IVtxReaderFacade* reader, bool force_apply);

    VtxServices::ReaderProfileCacheState profile_cache_;
    std::shared_ptr<InspectorSession> inspector_session_;
    int cache_window_backward_ = 2;
    int cache_window_forward_ = 2;
    int last_applied_backward_ = -1;
    int last_applied_forward_ = -1;
    const void* last_applied_reader_ = nullptr;
};
