/**
* @file vtx_loader.h
 * 
 * @brief This file provides generic functions to use different loading methods from a centralized place
 * 
 * @author Zenos Interactive
 */


#pragma once
#include <vtx/common/adapters/json/json_adapter.h>

#include "universal_deserializer.h"
#include "vtx/common/vtx_error_policy.h"

namespace VTX {

    template <typename T, typename Policy = VTX::SilentErrorPolicy>
    T Load(const VTX::JsonAdapter& reader) {
        return UniversalDeserializer<Policy>::template Load<T>(reader);
    }


    template <typename T, typename FbPointer>
    T Load(const FbPointer* fb_data) {
        return FlatBufferAdapter::Load<T>(fb_data);
    }
} // namespace VTX
