/**
 * @file bucket_type_sort.h
 * @brief Shared bucket reordering used by the formatter policies before serialization.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <algorithm>
#include <vector>

#include "vtx/common/vtx_types.h"

namespace VTX {
    namespace Serialization {

        /**
         * @brief Copies @p src into @p dst with entities grouped by entity_type_id,
         *        rebuilding type_ranges so readers can slice entities per type.
         * @details Buckets with no entities (or no valid type ids) are copied verbatim.
         */
        inline void SortBucketByTypeId(const VTX::Bucket& src, VTX::Bucket& dst) {
            const auto& entities = src.entities;
            const auto& ids = src.unique_ids;

            if (entities.empty()) {
                dst = src;
                return;
            }

            int32_t max_type = -1;
            for (const auto& ent : entities) {
                max_type = std::max(ent.entity_type_id, max_type);
            }

            if (max_type < 0) {
                dst = src;
                return;
            }

            std::vector<std::vector<size_t>> indices_by_type(max_type + 1);
            for (size_t i = 0; i < entities.size(); ++i) {
                int32_t t_id = entities[i].entity_type_id;
                if (t_id >= 0 && t_id <= max_type) {
                    indices_by_type[t_id].push_back(i);
                }
            }

            dst.type_ranges.assign(max_type + 1, {0, 0});
            dst.entities.reserve(entities.size());
            dst.unique_ids.reserve(ids.size());

            int32_t current_index = 0;

            for (int32_t type_id = 0; type_id <= max_type; ++type_id) {
                const auto& indices = indices_by_type[type_id];
                dst.type_ranges[type_id].start_index = current_index;
                dst.type_ranges[type_id].count = static_cast<int32_t>(indices.size());

                for (size_t orig_idx : indices) {
                    dst.entities.push_back(entities[orig_idx]);
                    if (orig_idx < ids.size()) {
                        dst.unique_ids.push_back(ids[orig_idx]);
                    }
                    current_index++;
                }
            }
        }

    } // namespace Serialization
} // namespace VTX
