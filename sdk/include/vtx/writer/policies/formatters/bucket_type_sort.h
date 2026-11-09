/**
 * @file bucket_type_sort.h
 * @brief Shared bucket reordering used by the formatter policies before serialization.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <algorithm>
#include <utility>
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


        /**
         * @brief The same result as SortBucketByTypeId(const Bucket&, Bucket&), moving @p src's entities and
         *        ids into @p dst instead of copying them. @p src is left valid but unspecified.
         * @details A bucket whose entities are already grouped by type (non-decreasing, valid type ids) --
         *          the usual case for a writer that builds a frame type by type -- is moved over whole, with
         *          only its type_ranges rebuilt.
         */
        inline void SortBucketByTypeId(VTX::Bucket&& src, VTX::Bucket& dst) {
            auto& entities = src.entities;
            auto& ids = src.unique_ids;

            if (entities.empty()) {
                dst = std::move(src);
                return;
            }

            int32_t max_type = -1;
            bool grouped = true;
            int32_t previous = -1;
            for (const auto& ent : entities) {
                max_type = std::max(ent.entity_type_id, max_type);
                if (ent.entity_type_id < previous)
                    grouped = false;
                previous = ent.entity_type_id;
            }

            if (max_type < 0) {
                dst = std::move(src);
                return;
            }

            dst.type_ranges.assign(max_type + 1, {0, 0});

            // Already grouped, every type id valid: the order stays, so the entities move over as they are.
            if (grouped && entities.front().entity_type_id >= 0) {
                for (size_t i = 0; i < entities.size(); ++i) {
                    const int32_t t_id = entities[i].entity_type_id;
                    if (dst.type_ranges[t_id].count == 0)
                        dst.type_ranges[t_id].start_index = static_cast<int32_t>(i);
                    ++dst.type_ranges[t_id].count;
                }
                // Types with no entity start where the next present type starts, as the copying sort sets them.
                int32_t next_start = static_cast<int32_t>(entities.size());
                for (int32_t type_id = max_type; type_id >= 0; --type_id) {
                    if (dst.type_ranges[type_id].count == 0)
                        dst.type_ranges[type_id].start_index = next_start;
                    else
                        next_start = dst.type_ranges[type_id].start_index;
                }
                if (ids.size() > entities.size())
                    ids.resize(entities.size());
                dst.entities = std::move(entities);
                dst.unique_ids = std::move(ids);
                return;
            }

            std::vector<std::vector<size_t>> indices_by_type(max_type + 1);
            for (size_t i = 0; i < entities.size(); ++i) {
                int32_t t_id = entities[i].entity_type_id;
                if (t_id >= 0 && t_id <= max_type) {
                    indices_by_type[t_id].push_back(i);
                }
            }

            dst.entities.reserve(entities.size());
            dst.unique_ids.reserve(ids.size());

            int32_t current_index = 0;
            for (int32_t type_id = 0; type_id <= max_type; ++type_id) {
                const auto& indices = indices_by_type[type_id];
                dst.type_ranges[type_id].start_index = current_index;
                dst.type_ranges[type_id].count = static_cast<int32_t>(indices.size());

                for (size_t orig_idx : indices) {
                    dst.entities.push_back(std::move(entities[orig_idx]));
                    if (orig_idx < ids.size()) {
                        dst.unique_ids.push_back(std::move(ids[orig_idx]));
                    }
                    current_index++;
                }
            }
        }

    } // namespace Serialization
} // namespace VTX
