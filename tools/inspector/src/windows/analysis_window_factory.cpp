#include "windows/analysis_window_factory.h"

#include "windows/entity_lifetime_window.h"
#include "windows/unique_properties_window.h"
#include "windows/track_property_window.h"

namespace AnalysisWindowFactory {

std::shared_ptr<IGuiLayer> CreateEntityLifeTimeWindow(
    std::shared_ptr<InspectorSession> session, int instance_id)
{
    return std::make_shared<EntityLifeTimeWindow>(std::move(session), instance_id);
}

std::shared_ptr<IGuiLayer> CreateUniquePropertiesWindow(
    std::shared_ptr<InspectorSession> session, int instance_id)
{
    return std::make_shared<UniquePropertiesWindow>(std::move(session), instance_id);
}

std::shared_ptr<IGuiLayer> CreateTrackPropertyWindow(
    std::shared_ptr<InspectorSession> session, int instance_id)
{
    return std::make_shared<TrackPropertyWindow>(std::move(session), instance_id);
}

bool IsAnalysisWindowOpen(const std::shared_ptr<IGuiLayer>& layer) {
    // Check each concrete type — they all inherit IsOpen() from the project's
    // gui_window base class, which we can't name here due to the imgui collision.
    if (auto* w = dynamic_cast<EntityLifeTimeWindow*>(layer.get()))
        return w->IsOpen();
    if (auto* w = dynamic_cast<UniquePropertiesWindow*>(layer.get()))
        return w->IsOpen();
    if (auto* w = dynamic_cast<TrackPropertyWindow*>(layer.get()))
        return w->IsOpen();
    return false;
}

} // namespace AnalysisWindowFactory
