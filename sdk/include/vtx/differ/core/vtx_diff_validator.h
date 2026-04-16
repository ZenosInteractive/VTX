#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"
#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/differ/core/vtx_patch.h"

namespace VtxDiff {

inline uint64_t StableHash64(std::string_view s);

template <typename TNodeView>
requires CBinaryNodeView<TNodeView>
class DiffValidator {
public:
    static bool ValidatePatch(const TNodeView& NodeA,
                              const TNodeView& NodeB,
                              const PatchIndex& Patch,
                              const DiffOptions& Opt)
    {
        std::cout << "\n[VALIDATOR] Starting Patch Validator...\n";
        bool allValid = true;
        int checkedOps = 0;

        for (const auto& Op : Patch.operations) {
            const bool existsA = PatchAccessor<TNodeView>::Exists(NodeA, Op);
            const bool existsB = PatchAccessor<TNodeView>::Exists(NodeB, Op);
            const auto bytesA = PatchAccessor<TNodeView>::GetRawBytes(NodeA, Op);
            const auto bytesB = PatchAccessor<TNodeView>::GetRawBytes(NodeB, Op);

            switch (Op.Operation) {
            case DiffOperation::Replace:
                if (!existsA || !existsB) {
                    std::cerr << "[FAIL] Replace on missing data. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                } else if (bytesA.empty() || bytesB.empty()) {
                    std::cerr << "[FAIL] Replace without readable bytes. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                } else if (AreBytesEqual(bytesA, bytesB, Opt)) {
                    std::cerr << "[FAIL] False positive replace. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                break;

            case DiffOperation::ReplaceRange:
                if (!existsA || !existsB) {
                    std::cerr << "[FAIL] ReplaceRange on missing data. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                } else if (bytesA.empty() || bytesB.empty()) {
                    std::cerr << "[FAIL] ReplaceRange without readable bytes. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                } else if (AreBytesEqual(bytesA, bytesB, Opt)) {
                    std::cerr << "[FAIL] False positive replace range. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                break;

            case DiffOperation::Add:
                if (existsA) {
                    std::cerr << "[FAIL] Add op but node already exists in A. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                if (!existsB) {
                    std::cerr << "[FAIL] Add op but node missing in B. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                break;

            case DiffOperation::Remove:
                if (!existsA) {
                    std::cerr << "[FAIL] Remove op but node missing in A. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                if (existsB) {
                    std::cerr << "[FAIL] Remove op but node still exists in B. Path: " << DecodePath(Patch, Op) << "\n";
                    allValid = false;
                }
                break;
            }

            checkedOps++;
        }

        if (allValid) {
            std::cout << "[PASS] " << checkedOps << " operations verified successfully.\n";
        } else {
            std::cout << "[WARN] Validation found inconsistencies.\n";
        }

        return allValid;
    }

    static void PrintPatch(const PatchIndex& Patch, size_t max_ops_to_print = 50) {
        std::cout << "\n================================================================================\n";
        std::cout << "                              PATCH DEBUG PRINTER                               \n";
        std::cout << "================================================================================\n";
        std::cout << "Total Ops: " << Patch.operations.size() << "\n";
        std::cout << "--------------------------------------------------------------------------------\n";

        const size_t limit = std::min(Patch.operations.size(), max_ops_to_print);
        for (size_t i = 0; i < limit; ++i) {
            const auto& op = Patch.operations[i];

            std::string op_name;
            switch (op.Operation) {
            case DiffOperation::Add:          op_name = "[ADD]"; break;
            case DiffOperation::Remove:       op_name = "[REMOVE]"; break;
            case DiffOperation::Replace:      op_name = "[REPLACE]"; break;
            case DiffOperation::ReplaceRange: op_name = "[REPLACE RANGE]"; break;
            }

            std::cout << std::left << std::setw(17) << op_name
                      << " | Field: " << std::setw(22) << TypeToFieldName(op.ContainerType);
            if (op.Operation == DiffOperation::ReplaceRange) {
                std::cout << " | Count: " << op.ReplaceRangeCount;
            }
            std::cout << " -> " << DecodePath(Patch, op) << "\n";
        }

        if (Patch.operations.size() > limit) {
            std::cout << "\n  " << (Patch.operations.size() - limit) << " hidden ops.\n";
        }
        std::cout << "================================================================================\n\n";
    }

private:
    static bool AreBytesEqual(std::span<const std::byte> BytesA, std::span<const std::byte> BytesB, const DiffOptions& Opt) {
        if (BytesA.size() != BytesB.size()) {
            return false;
        }

        bool different = false;
        if (Opt.compare_floats_with_epsilon && BytesA.size() == BytesB.size()) {
            if (BytesA.size() == sizeof(float)) {
                float fa = 0.0f;
                float fb = 0.0f;
                std::memcpy(&fa, BytesA.data(), sizeof(float));
                std::memcpy(&fb, BytesB.data(), sizeof(float));
                different = std::abs(fa - fb) > Opt.float_epsilon;
            } else if (BytesA.size() == sizeof(double)) {
                double da = 0.0;
                double db = 0.0;
                std::memcpy(&da, BytesA.data(), sizeof(double));
                std::memcpy(&db, BytesB.data(), sizeof(double));
                different = std::abs(da - db) > static_cast<double>(Opt.float_epsilon);
            } else {
                different = std::memcmp(BytesA.data(), BytesB.data(), BytesA.size()) != 0;
            }
        } else {
            different = std::memcmp(BytesA.data(), BytesB.data(), BytesA.size()) != 0;
        }

        return !different;
    }

    static std::string DecodePath(const PatchIndex& Patch, const DiffIndexOp& Op) {
        if (Op.Path.size() == 0) {
            return "root";
        }

        std::string result = "root";
        for (size_t i = 0; i < Op.Path.size(); ++i) {
            const int32_t value = Op.Path.indices[i];

            if (i == 0) {
                const auto type = static_cast<EVTXContainerType>(value);
                result += (type == EVTXContainerType::AnyStructArrays) ? ".entities" : "." + TypeToFieldName(type);
                continue;
            }

            if (i == 1 && Op.Path.indices[0] == static_cast<int32_t>(EVTXContainerType::AnyStructArrays)) {
                auto it = Patch.actor_id_by_key.find(value);
                if (it != Patch.actor_id_by_key.end()) {
                    result += "[\"" + it->second + "\"]";
                } else if (!Op.ActorId.empty()) {
                    result += "[\"" + Op.ActorId + "\"]";
                } else {
                    result += "[Hash:" + std::to_string(value) + "]";
                }
                continue;
            }

            const int32_t previous = Op.Path.indices[i - 1];
            if (previous == static_cast<int32_t>(EVTXContainerType::MapProperties) ||
                previous == static_cast<int32_t>(EVTXContainerType::MapArrays)) {
                if (!Op.MapKey.empty()) {
                    result += "{\"" + Op.MapKey + "\"}";
                } else {
                    result += "{Hash:" + std::to_string(value) + "}";
                }
                continue;
            }

            if (previous >= static_cast<int32_t>(EVTXContainerType::BoolProperties) &&
                previous <= static_cast<int32_t>(EVTXContainerType::MapArrays)) {
                result += "[" + std::to_string(value) + "]";
                continue;
            }

            const auto type = static_cast<EVTXContainerType>(value);
            const std::string field_name = TypeToFieldName(type);
            if (!field_name.empty()) {
                result += "." + field_name;
            } else {
                result += "[" + std::to_string(value) + "]";
            }
        }

        return result;
    }
};

} // namespace VtxDiff
