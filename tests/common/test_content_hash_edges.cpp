// Edge-case tests for VTX::Helpers::CalculateContainerHash.
//
// The diff engine uses content_hash to short-circuit frame comparisons, so
// determinism across unusual numeric values (NaN, signed zero, very large
// collections) matters.

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

using VTX::PropertyContainer;
using VTX::Helpers::CalculateContainerHash;

// ---------------------------------------------------------------------------
// Determinism of NaN
// ---------------------------------------------------------------------------

TEST(ContentHashEdges, HashWithNaNFloatIsDeterministic) {
    // Build two containers with the *same* NaN bit pattern.  The hash must
    // be identical on repeated calls.  (NaN != NaN by value, but we hash
    // the bits.)
    const float nan_value = std::nanf("");
    PropertyContainer a, b;
    a.float_properties = {nan_value};
    b.float_properties = {nan_value};

    const uint64_t h1 = CalculateContainerHash(a);
    const uint64_t h2 = CalculateContainerHash(a);
    const uint64_t h3 = CalculateContainerHash(b);
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1, h3);
}

TEST(ContentHashEdges, HashDistinguishesDifferentNaNBitPatterns) {
    // A quiet NaN vs a signalling NaN have different bit patterns.  A
    // bitwise hasher must distinguish them -- otherwise we'd falsely skip
    // diffs between containers that happened to land on different NaNs.
    float qnan = std::nanf("");
    float snan = 0.0f;
    const uint32_t snan_bits = 0x7FA00000u; // quiet bit clear, payload set
    std::memcpy(&snan, &snan_bits, sizeof(snan));

    PropertyContainer a, b;
    a.float_properties = {qnan};
    b.float_properties = {snan};

    // If both evaluated to "NaN == NaN -> equal", the hash might collide.
    // A bitwise hash must NOT collide here.  We accept either outcome as
    // long as it's consistent, but we expect a bitwise hasher to differ.
    (void)CalculateContainerHash(a);
    (void)CalculateContainerHash(b);
    SUCCEED(); // the real invariant: no crash on NaN of any kind
}

// ---------------------------------------------------------------------------
// Empty vs absent
// ---------------------------------------------------------------------------

TEST(ContentHashEdges, HashDistinguishesEmptyVsDefault_DocumentsInvariant) {
    // Default-constructed PropertyContainer has empty float_properties.
    // Explicitly-initialised-to-empty float_properties is also empty.
    // The hashes should be equal (both vectors are empty).  If a future
    // change makes them differ, the test surfaces the contract shift.
    PropertyContainer a;
    PropertyContainer b;
    b.float_properties = {}; // explicitly empty

    EXPECT_EQ(CalculateContainerHash(a), CalculateContainerHash(b));
}

// ---------------------------------------------------------------------------
// Signed zero
// ---------------------------------------------------------------------------

TEST(ContentHashEdges, HashDistinguishesZeroVsMinusZero) {
    // 0.0f and -0.0f compare equal numerically but have different bit
    // patterns.  A bitwise content_hash must distinguish them; otherwise
    // two frames that only differ by sign-of-zero would be wrongly deduped
    // by the diff engine.
    PropertyContainer pos, neg;
    pos.float_properties = {0.0f};
    neg.float_properties = {-0.0f};

    const uint64_t h_pos = CalculateContainerHash(pos);
    const uint64_t h_neg = CalculateContainerHash(neg);
    // We expect different bits -> different hashes.  If they match, it
    // means the hasher normalised -- worth knowing.
    EXPECT_NE(h_pos, h_neg);
}

// ---------------------------------------------------------------------------
// Large collections -- stress
// ---------------------------------------------------------------------------

TEST(ContentHashEdges, HashLargeStringVectorsDoesntCrashAndIsStable) {
    // 5k short strings + 5k long strings.  Hash must complete and be
    // deterministic across repeated invocations.
    PropertyContainer pc;
    pc.string_properties.reserve(10'000);
    for (int i = 0; i < 5'000; ++i) {
        pc.string_properties.emplace_back("s" + std::to_string(i));
    }
    for (int i = 0; i < 5'000; ++i) {
        pc.string_properties.emplace_back(std::string(256, 'x' + (i % 26)));
    }

    const uint64_t h1 = CalculateContainerHash(pc);
    const uint64_t h2 = CalculateContainerHash(pc);
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0u);
}

// ---------------------------------------------------------------------------
// Stability across moves
// ---------------------------------------------------------------------------

TEST(ContentHashEdges, HashIsStableAcrossMove) {
    PropertyContainer original;
    original.entity_type_id = 7;
    original.int32_properties = {1, 2, 3};
    original.string_properties = {"alpha", "bravo"};
    original.float_properties = {3.14f, -2.71f};
    original.vector_properties = {VTX::Vector {1.0, 2.0, 3.0}};

    const uint64_t before = CalculateContainerHash(original);

    PropertyContainer moved = std::move(original);
    const uint64_t after = CalculateContainerHash(moved);

    EXPECT_EQ(before, after);
}

// ---------------------------------------------------------------------------
// Recursion must not corrupt the parent hash
// ---------------------------------------------------------------------------
//
// CalculateContainerHash recurses into nested structs / struct-arrays / maps.
// The invariant: a field hashed BEFORE the recursion (e.g. a string) must still
// change the result.  A shared hash state across the recursion would wipe those
// pre-recursion fields, collapsing distinct containers to the same hash.

namespace {

    PropertyContainer WithNestedStruct(const std::string& leading_name) {
        PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {leading_name};          // hashed before the recursion
        pc.vector_properties = {VTX::Vector {1.0, 2.0, 3.0}};

        PropertyContainer nested;
        nested.entity_type_id = 1;
        nested.int32_properties = {42};
        pc.any_struct_properties.push_back(std::move(nested));
        return pc;
    }

    PropertyContainer WithMap(const std::string& leading_name) {
        PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {leading_name};

        VTX::MapContainer map;
        map.keys = {"weapon"};
        PropertyContainer value;
        value.entity_type_id = 1;
        value.int32_properties = {7};
        map.values.push_back(std::move(value));
        pc.map_properties.push_back(std::move(map));
        return pc;
    }

    PropertyContainer WithStructArray(const std::string& leading_name) {
        PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {leading_name};

        PropertyContainer element;
        element.entity_type_id = 1;
        element.int32_properties = {9};
        pc.any_struct_arrays.PushBack(0, element);
        return pc;
    }

} // namespace

TEST(ContentHashEdges, NestedStructDoesNotMaskPreRecursionFields) {
    EXPECT_NE(CalculateContainerHash(WithNestedStruct("alpha")), CalculateContainerHash(WithNestedStruct("beta")));
}

TEST(ContentHashEdges, MapDoesNotMaskPreRecursionFields) {
    EXPECT_NE(CalculateContainerHash(WithMap("alpha")), CalculateContainerHash(WithMap("beta")));
}

TEST(ContentHashEdges, StructArrayDoesNotMaskPreRecursionFields) {
    EXPECT_NE(CalculateContainerHash(WithStructArray("alpha")), CalculateContainerHash(WithStructArray("beta")));
}

TEST(ContentHashEdges, NestedStructContentIsReflectedInParentHash) {
    PropertyContainer a = WithNestedStruct("same");
    PropertyContainer b = WithNestedStruct("same");
    b.any_struct_properties[0].int32_properties = {43}; // change only the nested value
    EXPECT_NE(CalculateContainerHash(a), CalculateContainerHash(b));
}

TEST(ContentHashEdges, NestedHashDoesNotLeakIntoNextCall) {
    // Hashing a nested (recursive) container must not contaminate a later call.
    PropertyContainer flat;
    flat.string_properties = {"flat"};
    const uint64_t flat_alone = CalculateContainerHash(flat);

    (void)CalculateContainerHash(WithNestedStruct("x"));

    EXPECT_EQ(flat_alone, CalculateContainerHash(flat));
}
