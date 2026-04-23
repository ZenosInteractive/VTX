#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>
#include <iostream>

#include "vtx_types.h"

namespace VTX {
    /**
     * @brief Equivalent to UE's FBoneCompressionSettings.
     * Defines the ranges and precision modes for packing the Transform.
     */
    struct BoneCompressionSettings {
        float position_minimum = -10000.0f;
        float position_range = 20000.0f; // Max - Min

        float scale_minimum = 0.0f;
        float scale_range = 10.0f;

        Vector default_scale = {1.0, 1.0, 1.0};
        bool use_high_precision_position_no_scale = false;

        inline float GetPositionRange() const { return position_range; }
        inline float GetScaleRange() const { return scale_range; }
    };

    /**
     * OptimizedBoneData
     * A 16-byte (128-bit) container for a full Transform (Quat, Pos, Scale).
     * Layout: [41 Bits Rotation] [87 Bits Payload]
     */
    struct alignas(16) OptimizedBoneData {
        /** Raw bit-storage. Exactly 16 bytes. 
         * alignas(16) ensures this can be loaded into SIMD registers efficiently. */
        uint32_t packed_transform[4];

        /** Packs a standard VTX::Transform into the 16-byte bitstream. */
        void Pack(const Transform& SourceTransform, const BoneCompressionSettings& Settings) {
            // Reset all bits to zero
            packed_transform[0] = packed_transform[1] = packed_transform[2] = packed_transform[3] = 0;

            // --- SECTION 1: ROTATION (Fixed 41 Bits) ---
            Quat NormalizedQuat = SourceTransform.rotation;

            // Normalize Quat
            float invLen = 1.0f / std::sqrt(NormalizedQuat.x * NormalizedQuat.x + NormalizedQuat.y * NormalizedQuat.y +
                                            NormalizedQuat.z * NormalizedQuat.z + NormalizedQuat.w * NormalizedQuat.w);
            NormalizedQuat.x *= invLen;
            NormalizedQuat.y *= invLen;
            NormalizedQuat.z *= invLen;
            NormalizedQuat.w *= invLen;

            // Ensure W is always positive (Quaternions are double-cover, -Q == Q)
            if (NormalizedQuat.w < 0.0f) {
                NormalizedQuat.x *= -1.0f;
                NormalizedQuat.y *= -1.0f;
                NormalizedQuat.z *= -1.0f;
                NormalizedQuat.w *= -1.0f;
            }

            const std::array<float, 4> Components = {NormalizedQuat.x, NormalizedQuat.y, NormalizedQuat.z,
                                                     NormalizedQuat.w};

            // Find the index of the largest absolute component
            int32_t LargestIndex = 0;
            float LargestValue = std::abs(Components[0]);
            for (int32_t i = 1; i < 4; i++) {
                if (std::abs(Components[i]) > LargestValue) {
                    LargestValue = std::abs(Components[i]);
                    LargestIndex = i;
                }
            }

            // If the largest component is negative, flip the quat (helps reconstruction sign)
            const float SignMultiplier = (Components[LargestIndex] < 0.0f) ? -1.0f : 1.0f;

            // Pack the index (2 bits) and the other 3 components (13 bits each)
            constexpr float RotationPackFactor = 5791.9103f;
            const uint32_t PackedRotationIndex = static_cast<uint32_t>(LargestIndex);

            auto PackComponent = [&](int offset) -> uint32_t {
                float comp = Components[(LargestIndex + offset) % 4] * SignMultiplier;
                return std::clamp(static_cast<uint32_t>(std::round((comp + 0.707107f) * RotationPackFactor)), 0u,
                                  8191u);
            };

            const uint32_t Quat1 = PackComponent(1);
            const uint32_t Quat2 = PackComponent(2);
            const uint32_t Quat3 = PackComponent(3);

            // Store Rotation in the first 41 bits
            packed_transform[0] = (PackedRotationIndex) | (Quat1 << 2) | (Quat2 << 15) | (Quat3 << 28);
            packed_transform[1] = (Quat3 >> 4); // The remaining 9 bits of Quat3

            // --- SECTION 2: POSITION & SCALE (Variable 87 Bits) ---
            const Vector& Location = SourceTransform.translation;

            const float PosRange = std::max(0.0001f, Settings.GetPositionRange());
            const float InvPosRange = 1.0f / PosRange;

            if (Settings.use_high_precision_position_no_scale) {
                // Mode: 21 bits per Position axis (0 to 2,097,151)
                const float PosScaleFactor = 2097151.0f * InvPosRange;

                auto PackPos = [&](double locAxis) -> uint32_t {
                    return std::clamp(static_cast<uint32_t>(std::round(
                                          (static_cast<float>(locAxis) - Settings.position_minimum) * PosScaleFactor)),
                                      0u, 2097151u);
                };

                const uint32_t PackedPosX = PackPos(Location.x);
                const uint32_t PackedPosY = PackPos(Location.y);
                const uint32_t PackedPosZ = PackPos(Location.z);

                packed_transform[1] |= (PackedPosX << 9);
                packed_transform[2] = PackedPosY | ((PackedPosZ & 0x7FF) << 21);
                packed_transform[3] = (PackedPosZ >> 11);
            } else {
                // Mode: 16 bits Position (0 to 65535) + 8 bits Scale (0 to 255)
                const float PosScaleFactor = 65535.0f * InvPosRange;

                auto PackPos = [&](double locAxis) -> uint32_t {
                    return std::clamp(static_cast<uint32_t>(std::round(
                                          (static_cast<float>(locAxis) - Settings.position_minimum) * PosScaleFactor)),
                                      0u, 65535u);
                };

                const uint32_t PackedPosX = PackPos(Location.x);
                const uint32_t PackedPosY = PackPos(Location.y);
                const uint32_t PackedPosZ = PackPos(Location.z);

                const Vector& Scale = SourceTransform.scale;
                const float ScaleRange = std::max(0.0001f, Settings.GetScaleRange());
                const float ScalePackFactor = 255.0f * (1.0f / ScaleRange);

                auto PackScale = [&](double scaleAxis) -> uint32_t {
                    return std::clamp(static_cast<uint32_t>(std::round(
                                          (static_cast<float>(scaleAxis) - Settings.scale_minimum) * ScalePackFactor)),
                                      0u, 255u);
                };

                const uint32_t PackedScaleX = PackScale(Scale.x);
                const uint32_t PackedScaleY = PackScale(Scale.y);
                const uint32_t PackedScaleZ = PackScale(Scale.z);

                packed_transform[1] |= (PackedPosX << 9) | ((PackedPosY & 0x7F) << 25);
                packed_transform[2] = (PackedPosY >> 7) | (PackedPosZ << 9) | ((PackedScaleX & 0x7F) << 25);
                packed_transform[3] = (PackedScaleX >> 7) | (PackedScaleY << 1) | (PackedScaleZ << 9);
            }
        }

        /** Unpacks the 16-byte buffer into a usable VTX::Transform. */
        void Unpack(const BoneCompressionSettings& Settings, Transform& OutTransform) const {
            // --- STEP 1: EXTRACT ROTATION ---
            const uint32_t QuatIndex = packed_transform[0] & 0x3;
            const uint32_t RawQuat1 = (packed_transform[0] >> 2) & 0x1FFF;
            const uint32_t RawQuat2 = (packed_transform[0] >> 15) & 0x1FFF;
            const uint32_t RawQuat3 = ((packed_transform[0] >> 28) & 0xF) | ((packed_transform[1] & 0x1FF) << 4);

            std::array<float, 4> QuatComponents = {0.0f, 0.0f, 0.0f, 0.0f};

            constexpr float QuatUnpackFactor = 0.000172654f;
            auto DequantizeQuat = [](const uint32_t Value) {
                return (static_cast<float>(Value) * QuatUnpackFactor) - 0.707107f;
            };

            QuatComponents[(QuatIndex + 1) % 4] = DequantizeQuat(RawQuat1);
            QuatComponents[(QuatIndex + 2) % 4] = DequantizeQuat(RawQuat2);
            QuatComponents[(QuatIndex + 3) % 4] = DequantizeQuat(RawQuat3);

            // Reconstruct the largest component using the property X^2 + Y^2 + Z^2 + W^2 = 1
            const float SquaredSum = QuatComponents[(QuatIndex + 1) % 4] * QuatComponents[(QuatIndex + 1) % 4] +
                                     QuatComponents[(QuatIndex + 2) % 4] * QuatComponents[(QuatIndex + 2) % 4] +
                                     QuatComponents[(QuatIndex + 3) % 4] * QuatComponents[(QuatIndex + 3) % 4];

            QuatComponents[QuatIndex] = std::sqrt(std::max(0.0f, 1.0f - SquaredSum));

            OutTransform.rotation = {QuatComponents[0], QuatComponents[1], QuatComponents[2], QuatComponents[3]};

            // --- STEP 2: EXTRACT POSITION & SCALE ---
            Vector OutPosition;
            Vector OutScale = Settings.default_scale;
            const float PosRange = Settings.GetPositionRange();

            if (Settings.use_high_precision_position_no_scale) {
                // 21-bit Extraction
                const uint32_t RawPosX = (packed_transform[1] >> 9) & 0x1FFFFF;
                const uint32_t RawPosY = (packed_transform[2] & 0x1FFFFF);
                const uint32_t RawPosZ = ((packed_transform[2] >> 21) & 0x7FF) | ((packed_transform[3] & 0x3FF) << 11);

                const float PosUnpackFactor = PosRange * 0.000000476837f;

                OutPosition.x =
                    static_cast<double>(static_cast<float>(RawPosX) * PosUnpackFactor + Settings.position_minimum);
                OutPosition.y =
                    static_cast<double>(static_cast<float>(RawPosY) * PosUnpackFactor + Settings.position_minimum);
                OutPosition.z =
                    static_cast<double>(static_cast<float>(RawPosZ) * PosUnpackFactor + Settings.position_minimum);
            } else {
                // 16-bit Position + 8-bit Scale Extraction
                const uint32_t RawPosX = (packed_transform[1] >> 9) & 0xFFFF;
                const uint32_t RawPosY = ((packed_transform[1] >> 25) & 0x7F) | ((packed_transform[2] & 0x1FF) << 7);
                const uint32_t RawPosZ = (packed_transform[2] >> 9) & 0xFFFF;

                const float PosUnpackFactor = PosRange * 0.000015259021f;

                const uint32_t RawScaleX = ((packed_transform[2] >> 25) & 0x7F) | ((packed_transform[3] & 0x1) << 7);
                const uint32_t RawScaleY = (packed_transform[3] >> 1) & 0xFF;
                const uint32_t RawScaleZ = (packed_transform[3] >> 9) & 0xFF;

                OutPosition.x =
                    static_cast<double>(static_cast<float>(RawPosX) * PosUnpackFactor + Settings.position_minimum);
                OutPosition.y =
                    static_cast<double>(static_cast<float>(RawPosY) * PosUnpackFactor + Settings.position_minimum);
                OutPosition.z =
                    static_cast<double>(static_cast<float>(RawPosZ) * PosUnpackFactor + Settings.position_minimum);

                const float ScaleUnpackFactor = Settings.GetScaleRange() * 0.00392157f;

                OutScale.x =
                    static_cast<double>(static_cast<float>(RawScaleX) * ScaleUnpackFactor + Settings.scale_minimum);
                OutScale.y =
                    static_cast<double>(static_cast<float>(RawScaleY) * ScaleUnpackFactor + Settings.scale_minimum);
                OutScale.z =
                    static_cast<double>(static_cast<float>(RawScaleZ) * ScaleUnpackFactor + Settings.scale_minimum);
            }

            OutTransform.translation = OutPosition;
            OutTransform.scale = OutScale;
        }

        // C++ Standard I/O replacement for Unreal's FArchive
        friend std::ostream& operator<<(std::ostream& os, const OptimizedBoneData& data) {
            os.write(reinterpret_cast<const char*>(data.packed_transform), sizeof(data.packed_transform));
            return os;
        }

        friend std::istream& operator>>(std::istream& is, OptimizedBoneData& data) {
            is.read(reinterpret_cast<char*>(data.packed_transform), sizeof(data.packed_transform));
            return is;
        }
    };

    // Ensure it remains exactly 16 bytes for hardware alignment guarantees
    static_assert(sizeof(OptimizedBoneData) == 16, "OptimizedBoneData must be exactly 16 bytes");
} // namespace VTX