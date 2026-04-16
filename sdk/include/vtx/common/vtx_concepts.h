#pragma once
#include <concepts>
#include <stop_token>
#include <vector>
#include "vtx_types.h"


namespace VTX
{
    struct PropertyAddressCache;
    struct ChunkIndexData;
    struct SessionFooter;
    

    template <typename SchemaT>
    struct SchemaAdapter
    {
        static void BuildCache(const SchemaT&, PropertyAddressCache&) = delete;
    };


    //List of supported types in vtx 
    template<typename T>
        concept VtxScalarType = 
            std::same_as<T, bool> || std::same_as<T, int32_t> || std::same_as<T, int64_t> || std::same_as<T, uint8_t> ||
            std::same_as<T, float> || std::same_as<T, double> || std::same_as<T, std::string> ||
            std::same_as<T, VTX::Vector> || std::same_as<T, VTX::Quat> || 
            std::same_as<T, VTX::Transform> || std::same_as<T, VTX::FloatRange> || 
            std::same_as<T, VTX::PropertyContainer> || std::same_as<T, class EntityView>;

    //Concepts for arrays, uint8 is used for bools
    template<typename T>
    concept VtxArrayType = VtxScalarType<T> || std::same_as<T, uint8_t>;



    /*Concept that defines how a vtx writer policy must look*/
    template<typename T>
    concept IVtxWriterPolicy = requires(
        const typename T::HeaderType& header_config,
        const typename T::SchemaType& schema,
        std::vector<std::unique_ptr<typename T::FrameType>>& frames,
        int32_t chunk_index,
        bool is_compressed,
        const std::vector<ChunkIndexData>& seek_table,
        const SessionFooter& footer_data
    )
    {
        typename T::FrameType;
        typename T::SchemaType;
        typename T::HeaderType; 

         requires std::semiregular<typename T::HeaderType>;

        { T::GetMagicBytes() } -> std::convertible_to<std::string>;
        { T::SerializeHeader(header_config, schema) } -> std::convertible_to<std::string>;
        { T::SerializeChunk(frames, chunk_index,is_compressed) } -> std::convertible_to<std::string>;
        { T::SerializeFooter(seek_table, footer_data) } -> std::convertible_to<std::string>;
    };
    
    /*Concept that defines how a vtx reader_ policy must look*/
    template<typename T>
    concept IVtxReaderPolicy = requires(
        std::string buffer,
        const typename T::HeaderType& header,
        const typename T::FooterType& footer,
        std::vector<VTX::ChunkIndexEntry> index_table,
        VTX::GameTime::VTXGameTimes& game_times,
        int32_t chunk_idx,
        std::stop_token stop_token,
        std::vector<VTX::Frame>& out_native_frames,
        std::vector<uint8_t>& out_decompressed_blob,
        std::vector<std::span<const std::byte>>& out_raw_frames_spans)
    {
        typename T::HeaderType;
        typename T::FooterType;
        typename T::SchemaType;

        requires std::semiregular<typename T::HeaderType>;
        requires std::semiregular<typename T::FooterType>;
        requires std::semiregular<typename T::SchemaType>;

        { T::ParseHeader(buffer) } -> std::same_as<typename T::HeaderType>;
        { T::ParseFooter(buffer) } -> std::same_as<typename T::FooterType>;

        { T::GetTotalFrames(footer) } -> std::convertible_to<int32_t>;
        { T::GetSchema(header) } -> std::convertible_to<const typename T::SchemaType&>;
        { T::GetVTXHeader(header) } -> std::convertible_to<VTX::FileHeader>;
        { T::GetVTXFooter(footer) } -> std::convertible_to< VTX::FileFooter>;
        { T::ProcessChunkData(chunk_idx, buffer, stop_token,out_native_frames,out_decompressed_blob,out_raw_frames_spans) } -> std::same_as<void>;
    };


    //Schema adapter concept
    //Is mandatory to implement a BuildCache method , receives schema and cache and return void
    template <typename T>
    concept SchemaAdaptable = requires(const T& schema, PropertyAddressCache& cache) {
        { SchemaAdapter<T>::BuildCache(schema, cache) } -> std::same_as<void>;
    };
}
