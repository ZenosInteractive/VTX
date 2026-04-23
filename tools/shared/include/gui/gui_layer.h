#pragma once

class IGuiLayer {
public:
    virtual ~IGuiLayer() = default;
    virtual void OnUpdate() = 0;
    virtual void OnRender() = 0;
};