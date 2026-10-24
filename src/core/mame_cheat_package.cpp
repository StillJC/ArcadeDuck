// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "mame_cheat_package.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"

#include "LzmaDec.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace MAMECheats {
namespace {

enum class SevenZipReadStatus
{
  NotFound,
  Loaded,
  Error,
};

// Pugsy/MAME cheat packages use a single solid LZMA1 folder. Keep this reader limited to that
// package subset instead of bringing a second general-purpose archive stack into the emulator.
constexpr size_t START_HEADER_SIZE = 32;
constexpr u64 MAX_ARCHIVE_SIZE = 256 * 1024 * 1024;
constexpr u64 MAX_DECODED_HEADER_SIZE = 64 * 1024 * 1024;
constexpr u64 MAX_PACKAGE_UNPACKED_SIZE = 512 * 1024 * 1024;
constexpr u64 MAX_MEMBER_SIZE = 16 * 1024 * 1024;
constexpr u32 MAX_FILE_COUNT = 1000000;
constexpr u32 MAX_DICTIONARY_SIZE = 128 * 1024 * 1024;

constexpr u8 NID_END = 0x00;
constexpr u8 NID_HEADER = 0x01;
constexpr u8 NID_MAIN_STREAMS_INFO = 0x04;
constexpr u8 NID_FILES_INFO = 0x05;
constexpr u8 NID_PACK_INFO = 0x06;
constexpr u8 NID_UNPACK_INFO = 0x07;
constexpr u8 NID_SUBSTREAMS_INFO = 0x08;
constexpr u8 NID_SIZE = 0x09;
constexpr u8 NID_CRC = 0x0a;
constexpr u8 NID_FOLDER = 0x0b;
constexpr u8 NID_CODERS_UNPACK_SIZE = 0x0c;
constexpr u8 NID_NUM_UNPACK_STREAM = 0x0d;
constexpr u8 NID_EMPTY_STREAM = 0x0e;
constexpr u8 NID_NAME = 0x11;
constexpr u8 NID_ENCODED_HEADER = 0x17;

constexpr std::array<u8, 6> SEVEN_ZIP_SIGNATURE = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
constexpr std::array<u8, 3> LZMA_METHOD_ID = {0x03, 0x01, 0x01};

struct Digest
{
  bool defined = false;
  u32 value = 0;
};

struct StreamsInfo
{
  u64 pack_position = 0;
  u64 packed_size = 0;
  u64 unpacked_size = 0;
  std::array<u8, LZMA_PROPS_SIZE> lzma_properties = {};
  Digest unpacked_crc;
  std::vector<u64> substream_sizes;
  std::vector<Digest> substream_crcs;
};

struct MemberLocation
{
  bool found = false;
  u64 offset = 0;
  u64 size = 0;
  Digest crc;
};

class Reader
{
public:
  explicit Reader(std::span<const u8> data) : m_data(data) {}

  bool ReadByte(u8* value)
  {
    if (m_position >= m_data.size())
      return false;
    *value = m_data[m_position++];
    return true;
  }

  bool ReadUInt64(u64* value)
  {
    u8 first;
    if (!ReadByte(&first))
      return false;

    u64 result = 0;
    u8 mask = 0x80;
    for (u32 i = 0; i < 8; i++)
    {
      if ((first & mask) == 0)
      {
        result |= static_cast<u64>(first & (mask - 1)) << (i * 8);
        *value = result;
        return true;
      }

      u8 next;
      if (!ReadByte(&next))
        return false;
      result |= static_cast<u64>(next) << (i * 8);
      mask >>= 1;
    }

    *value = result;
    return true;
  }

  bool ReadLE32(u32* value)
  {
    std::span<const u8> bytes;
    if (!ReadSpan(sizeof(u32), &bytes))
      return false;
    *value = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
             (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
    return true;
  }

  bool ReadLE64(u64* value)
  {
    std::span<const u8> bytes;
    if (!ReadSpan(sizeof(u64), &bytes))
      return false;

    u64 result = 0;
    for (u32 i = 0; i < 8; i++)
      result |= static_cast<u64>(bytes[i]) << (i * 8);
    *value = result;
    return true;
  }

  bool ReadSpan(size_t size, std::span<const u8>* data)
  {
    if (size > (m_data.size() - m_position))
      return false;
    *data = m_data.subspan(m_position, size);
    m_position += size;
    return true;
  }

  bool ReadSubReader(u64 size, Reader* reader)
  {
    if (size > std::numeric_limits<size_t>::max())
      return false;
    std::span<const u8> data;
    if (!ReadSpan(static_cast<size_t>(size), &data))
      return false;
    *reader = Reader(data);
    return true;
  }

  bool Skip(u64 size)
  {
    if (size > std::numeric_limits<size_t>::max())
      return false;
    const size_t ssize = static_cast<size_t>(size);
    if (ssize > (m_data.size() - m_position))
      return false;
    m_position += ssize;
    return true;
  }

  size_t Remaining() const { return m_data.size() - m_position; }
  bool IsEmpty() const { return m_position == m_data.size(); }

private:
  std::span<const u8> m_data;
  size_t m_position = 0;
};

constexpr std::array<u32, 256> MakeCRC32Table()
{
  std::array<u32, 256> table = {};
  for (u32 i = 0; i < table.size(); i++)
  {
    u32 value = i;
    for (u32 bit = 0; bit < 8; bit++)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    table[i] = value;
  }
  return table;
}

constexpr std::array<u32, 256> CRC32_TABLE = MakeCRC32Table();

u32 ComputeCRC32(std::span<const u8> data)
{
  u32 crc = 0xffffffffu;
  for (const u8 value : data)
    crc = CRC32_TABLE[(crc ^ value) & 0xff] ^ (crc >> 8);
  return crc ^ 0xffffffffu;
}

bool GetArchiveSpan(std::span<const u8> archive, u64 relative_offset, u64 size, std::span<const u8>* data)
{
  if (relative_offset > (std::numeric_limits<u64>::max() - START_HEADER_SIZE))
    return false;
  const u64 absolute_offset = relative_offset + START_HEADER_SIZE;
  if (absolute_offset > archive.size() || size > (archive.size() - absolute_offset))
    return false;
  *data = archive.subspan(static_cast<size_t>(absolute_offset), static_cast<size_t>(size));
  return true;
}

void* LzmaAlloc(ISzAllocPtr, size_t size)
{
  return std::malloc(size);
}

void LzmaFree(ISzAllocPtr, void* address)
{
  std::free(address);
}

const ISzAlloc LZMA_ALLOCATOR = {LzmaAlloc, LzmaFree};

bool ValidateLzmaProperties(std::span<const u8> properties, Error* error)
{
  CLzmaProps decoded = {};
  if (properties.size() != LZMA_PROPS_SIZE ||
      LzmaProps_Decode(&decoded, properties.data(), static_cast<unsigned>(properties.size())) != SZ_OK)
  {
    Error::SetString(error, "Invalid LZMA properties in MAME cheat.7z.");
    return false;
  }
  if (decoded.dicSize > MAX_DICTIONARY_SIZE)
  {
    Error::SetString(error, "MAME cheat.7z requires an unsupported LZMA dictionary size.");
    return false;
  }
  return true;
}

bool DecodeWholeLzma(std::span<const u8> input, std::span<const u8> properties, u64 unpacked_size,
                     std::vector<u8>* output, Error* error)
{
  if (unpacked_size > MAX_DECODED_HEADER_SIZE || unpacked_size > std::numeric_limits<size_t>::max())
  {
    Error::SetString(error, "MAME cheat.7z metadata is too large.");
    return false;
  }
  if (!ValidateLzmaProperties(properties, error))
    return false;

  output->resize(static_cast<size_t>(unpacked_size));

  CLzmaDec decoder;
  LzmaDec_Construct(&decoder);
  const SRes alloc_result =
    LzmaDec_Allocate(&decoder, properties.data(), static_cast<unsigned>(properties.size()), &LZMA_ALLOCATOR);
  if (alloc_result != SZ_OK)
  {
    Error::SetString(error, "Failed to allocate the MAME cheat.7z LZMA decoder.");
    return false;
  }

  LzmaDec_Init(&decoder);
  SizeT input_size = input.size();
  SizeT output_size = output->size();
  ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
  const SRes decode_result = LzmaDec_DecodeToBuf(&decoder, output->data(), &output_size, input.data(), &input_size,
                                                  LZMA_FINISH_END, &status);
  LzmaDec_Free(&decoder, &LZMA_ALLOCATOR);

  if (decode_result != SZ_OK || output_size != output->size() || input_size != input.size() ||
      (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
  {
    Error::SetString(error, "Failed to decode MAME cheat.7z metadata.");
    return false;
  }
  return true;
}

// cheat.7z is solid, so locating one XML member still requires decoding from the start of the LZMA stream.
// Only the requested member range is retained to keep package lookup memory bounded.
bool DecodeLzmaRange(std::span<const u8> input, std::span<const u8> properties, u64 total_unpacked_size,
                     u64 range_offset, u64 range_size, std::string* output, Error* error)
{
  if (total_unpacked_size > MAX_PACKAGE_UNPACKED_SIZE || range_size > MAX_MEMBER_SIZE ||
      range_offset > total_unpacked_size || range_size > (total_unpacked_size - range_offset) ||
      range_size > std::numeric_limits<size_t>::max())
  {
    Error::SetString(error, "Invalid member bounds in MAME cheat.7z.");
    return false;
  }
  if (!ValidateLzmaProperties(properties, error))
    return false;

  output->resize(static_cast<size_t>(range_size));
  const u64 range_end = range_offset + range_size;

  CLzmaDec decoder;
  LzmaDec_Construct(&decoder);
  const SRes alloc_result =
    LzmaDec_Allocate(&decoder, properties.data(), static_cast<unsigned>(properties.size()), &LZMA_ALLOCATOR);
  if (alloc_result != SZ_OK)
  {
    Error::SetString(error, "Failed to allocate the MAME cheat.7z LZMA decoder.");
    return false;
  }

  LzmaDec_Init(&decoder);
  std::array<u8, 64 * 1024> buffer = {};
  size_t input_position = 0;
  u64 output_position = 0;
  ELzmaStatus final_status = LZMA_STATUS_NOT_SPECIFIED;

  while (output_position < range_end)
  {
    const size_t requested_output =
      static_cast<size_t>(std::min<u64>(buffer.size(), range_end - output_position));
    SizeT produced = requested_output;
    SizeT consumed = input.size() - input_position;
    const bool reaches_stream_end = (output_position + requested_output == total_unpacked_size);
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = LzmaDec_DecodeToBuf(&decoder, buffer.data(), &produced, input.data() + input_position, &consumed,
                                             reaches_stream_end ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);
    if (result != SZ_OK)
    {
      LzmaDec_Free(&decoder, &LZMA_ALLOCATOR);
      Error::SetString(error, "Failed to decode a member from MAME cheat.7z.");
      return false;
    }

    if (produced == 0 && consumed == 0)
    {
      LzmaDec_Free(&decoder, &LZMA_ALLOCATOR);
      Error::SetString(error, "MAME cheat.7z LZMA stream ended unexpectedly.");
      return false;
    }

    const u64 chunk_begin = output_position;
    const u64 chunk_end = output_position + produced;
    const u64 copy_begin = std::max(chunk_begin, range_offset);
    const u64 copy_end = std::min(chunk_end, range_end);
    if (copy_begin < copy_end)
    {
      const size_t source_offset = static_cast<size_t>(copy_begin - chunk_begin);
      const size_t destination_offset = static_cast<size_t>(copy_begin - range_offset);
      const size_t copy_size = static_cast<size_t>(copy_end - copy_begin);
      std::memcpy(output->data() + destination_offset, buffer.data() + source_offset, copy_size);
    }

    input_position += consumed;
    output_position += produced;
    final_status = status;
  }

  const bool decoded_to_end = (range_end == total_unpacked_size);
  const bool valid_end = !decoded_to_end ||
                         (input_position == input.size() &&
                          (final_status == LZMA_STATUS_FINISHED_WITH_MARK ||
                           final_status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK));
  LzmaDec_Free(&decoder, &LZMA_ALLOCATOR);

  if (!valid_end)
  {
    Error::SetString(error, "MAME cheat.7z LZMA stream did not finish cleanly.");
    return false;
  }
  return true;
}

bool ReadDigests(Reader* reader, u32 count, std::vector<Digest>* digests)
{
  digests->assign(count, {});
  if (count == 0)
    return true;

  u8 all_defined;
  if (!reader->ReadByte(&all_defined))
    return false;

  std::vector<u8> defined_bits;
  if (all_defined == 0)
  {
    const size_t byte_count = (static_cast<size_t>(count) + 7) / 8;
    defined_bits.resize(byte_count);
    for (size_t i = 0; i < byte_count; i++)
    {
      if (!reader->ReadByte(&defined_bits[i]))
        return false;
    }
  }

  for (u32 i = 0; i < count; i++)
  {
    const bool defined = (all_defined != 0) || ((defined_bits[i / 8] & (0x80 >> (i & 7))) != 0);
    if (!defined)
      continue;

    (*digests)[i].defined = true;
    if (!reader->ReadLE32(&(*digests)[i].value))
      return false;
  }
  return true;
}

bool ParsePackInfo(Reader* reader, StreamsInfo* streams)
{
  u64 num_pack_streams;
  if (!reader->ReadUInt64(&streams->pack_position) || !reader->ReadUInt64(&num_pack_streams) || num_pack_streams != 1)
    return false;

  bool have_size = false;
  for (;;)
  {
    u8 property;
    if (!reader->ReadByte(&property))
      return false;
    if (property == NID_END)
      break;

    if (property == NID_SIZE)
    {
      if (!reader->ReadUInt64(&streams->packed_size))
        return false;
      have_size = true;
    }
    else if (property == NID_CRC)
    {
      std::vector<Digest> ignored;
      if (!ReadDigests(reader, 1, &ignored))
        return false;
    }
    else
    {
      return false;
    }
  }

  return have_size;
}

bool ParseFolder(Reader* reader, StreamsInfo* streams)
{
  u64 num_coders;
  if (!reader->ReadUInt64(&num_coders) || num_coders != 1)
    return false;

  u8 flags;
  if (!reader->ReadByte(&flags))
    return false;
  const u32 method_id_size = flags & 0x0f;
  if (method_id_size != LZMA_METHOD_ID.size() || (flags & 0x80) != 0)
    return false;

  std::span<const u8> method_id;
  if (!reader->ReadSpan(method_id_size, &method_id) || !std::equal(method_id.begin(), method_id.end(), LZMA_METHOD_ID.begin()))
    return false;

  if ((flags & 0x10) != 0)
  {
    u64 num_inputs;
    u64 num_outputs;
    if (!reader->ReadUInt64(&num_inputs) || !reader->ReadUInt64(&num_outputs) || num_inputs != 1 || num_outputs != 1)
      return false;
  }

  if ((flags & 0x20) == 0)
    return false;

  u64 property_size;
  if (!reader->ReadUInt64(&property_size) || property_size != LZMA_PROPS_SIZE)
    return false;
  std::span<const u8> properties;
  if (!reader->ReadSpan(LZMA_PROPS_SIZE, &properties))
    return false;
  std::copy(properties.begin(), properties.end(), streams->lzma_properties.begin());
  return true;
}

bool ParseUnpackInfo(Reader* reader, StreamsInfo* streams)
{
  u8 property;
  if (!reader->ReadByte(&property) || property != NID_FOLDER)
    return false;

  u64 num_folders;
  u8 external;
  if (!reader->ReadUInt64(&num_folders) || num_folders != 1 || !reader->ReadByte(&external) || external != 0 ||
      !ParseFolder(reader, streams))
  {
    return false;
  }

  if (!reader->ReadByte(&property) || property != NID_CODERS_UNPACK_SIZE || !reader->ReadUInt64(&streams->unpacked_size))
    return false;

  if (!reader->ReadByte(&property))
    return false;
  if (property == NID_CRC)
  {
    std::vector<Digest> digests;
    if (!ReadDigests(reader, 1, &digests))
      return false;
    streams->unpacked_crc = digests[0];
    if (!reader->ReadByte(&property))
      return false;
  }

  return property == NID_END;
}

bool ParseSubStreamsInfo(Reader* reader, StreamsInfo* streams)
{
  u64 num_substreams = 1;
  std::vector<u64> explicit_sizes;
  std::vector<Digest> explicit_crcs;
  bool have_crcs = false;

  for (;;)
  {
    u8 property;
    if (!reader->ReadByte(&property))
      return false;
    if (property == NID_END)
      break;

    if (property == NID_NUM_UNPACK_STREAM)
    {
      if (!reader->ReadUInt64(&num_substreams) || num_substreams == 0 || num_substreams > MAX_FILE_COUNT)
        return false;
    }
    else if (property == NID_SIZE)
    {
      if (num_substreams == 0 || num_substreams > MAX_FILE_COUNT)
        return false;
      explicit_sizes.resize(static_cast<size_t>(num_substreams - 1));
      for (u64& size : explicit_sizes)
      {
        if (!reader->ReadUInt64(&size))
          return false;
      }
    }
    else if (property == NID_CRC)
    {
      const u64 digest_count = (num_substreams == 1 && streams->unpacked_crc.defined) ? 0 : num_substreams;
      if (digest_count > MAX_FILE_COUNT ||
          !ReadDigests(reader, static_cast<u32>(digest_count), &explicit_crcs))
      {
        return false;
      }
      have_crcs = true;
    }
    else
    {
      return false;
    }
  }

  streams->substream_sizes.clear();
  streams->substream_sizes.reserve(static_cast<size_t>(num_substreams));
  u64 size_sum = 0;
  for (const u64 size : explicit_sizes)
  {
    if (size > (streams->unpacked_size - size_sum))
      return false;
    streams->substream_sizes.push_back(size);
    size_sum += size;
  }
  if (num_substreams == 1 && explicit_sizes.empty())
  {
    streams->substream_sizes.push_back(streams->unpacked_size);
  }
  else
  {
    if (streams->substream_sizes.size() != (num_substreams - 1) || size_sum > streams->unpacked_size)
      return false;
    streams->substream_sizes.push_back(streams->unpacked_size - size_sum);
  }

  streams->substream_crcs.assign(static_cast<size_t>(num_substreams), {});
  if (num_substreams == 1 && streams->unpacked_crc.defined)
  {
    streams->substream_crcs[0] = streams->unpacked_crc;
  }
  else if (have_crcs)
  {
    if (explicit_crcs.size() != num_substreams)
      return false;
    streams->substream_crcs = std::move(explicit_crcs);
  }

  return true;
}

bool ParseStreamsInfo(Reader* reader, StreamsInfo* streams)
{
  u8 property;
  if (!reader->ReadByte(&property) || property != NID_PACK_INFO || !ParsePackInfo(reader, streams))
    return false;
  if (!reader->ReadByte(&property) || property != NID_UNPACK_INFO || !ParseUnpackInfo(reader, streams))
    return false;

  if (!reader->ReadByte(&property))
    return false;
  if (property == NID_SUBSTREAMS_INFO)
  {
    if (!ParseSubStreamsInfo(reader, streams) || !reader->ReadByte(&property))
      return false;
  }
  else
  {
    streams->substream_sizes = {streams->unpacked_size};
    streams->substream_crcs = {streams->unpacked_crc};
  }

  return property == NID_END;
}

bool GetEmptyStreamBit(std::span<const u8> bits, u32 index)
{
  return !bits.empty() && ((bits[index / 8] & (0x80 >> (index & 7))) != 0);
}

bool ReadUTF16NameMatches(Reader* reader, std::string_view filename, bool* matches)
{
  size_t character_index = 0;
  bool equal = true;
  for (;;)
  {
    u8 low;
    u8 high;
    if (!reader->ReadByte(&low) || !reader->ReadByte(&high))
      return false;
    if (low == 0 && high == 0)
      break;

    if (high != 0 || character_index >= filename.size() || low != static_cast<u8>(filename[character_index]))
      equal = false;
    character_index++;
  }

  *matches = equal && character_index == filename.size();
  return true;
}

bool ParseFilesInfo(Reader* reader, const StreamsInfo& streams, std::string_view filename, MemberLocation* member)
{
  u64 num_files_64;
  if (!reader->ReadUInt64(&num_files_64) || num_files_64 == 0 || num_files_64 > MAX_FILE_COUNT)
    return false;
  const u32 num_files = static_cast<u32>(num_files_64);

  std::span<const u8> empty_streams;
  std::span<const u8> names;

  for (;;)
  {
    u8 property;
    if (!reader->ReadByte(&property))
      return false;
    if (property == NID_END)
      break;

    u64 property_size;
    if (!reader->ReadUInt64(&property_size))
      return false;
    Reader property_reader{std::span<const u8>()};
    if (!reader->ReadSubReader(property_size, &property_reader))
      return false;

    if (property == NID_EMPTY_STREAM)
    {
      const size_t expected_size = (static_cast<size_t>(num_files) + 7) / 8;
      if (property_size != expected_size || !property_reader.ReadSpan(expected_size, &empty_streams) ||
          !property_reader.IsEmpty())
      {
        return false;
      }
    }
    else if (property == NID_NAME)
    {
      u8 external;
      if (!property_reader.ReadByte(&external) || external != 0 ||
          !property_reader.ReadSpan(property_reader.Remaining(), &names) || !property_reader.IsEmpty())
      {
        return false;
      }
    }
  }

  if (names.empty())
    return false;

  Reader name_reader(names);
  size_t stream_index = 0;
  u64 stream_offset = 0;
  for (u32 file_index = 0; file_index < num_files; file_index++)
  {
    bool name_matches = false;
    if (!ReadUTF16NameMatches(&name_reader, filename, &name_matches))
      return false;

    const bool empty = GetEmptyStreamBit(empty_streams, file_index);
    if (empty)
    {
      if (name_matches)
        return true;
      continue;
    }

    if (stream_index >= streams.substream_sizes.size())
      return false;
    const u64 stream_size = streams.substream_sizes[stream_index];
    if (stream_size > (streams.unpacked_size - stream_offset))
      return false;

    if (name_matches)
    {
      member->found = true;
      member->offset = stream_offset;
      member->size = stream_size;
      if (stream_index < streams.substream_crcs.size())
        member->crc = streams.substream_crcs[stream_index];
      return true;
    }

    stream_offset += stream_size;
    stream_index++;
  }

  return name_reader.IsEmpty() && stream_index == streams.substream_sizes.size();
}

bool ParseDecodedHeader(std::span<const u8> header, std::string_view filename, StreamsInfo* streams,
                        MemberLocation* member)
{
  Reader reader(header);
  u8 property;
  if (!reader.ReadByte(&property) || property != NID_HEADER)
    return false;
  if (!reader.ReadByte(&property) || property != NID_MAIN_STREAMS_INFO || !ParseStreamsInfo(&reader, streams))
    return false;
  if (!reader.ReadByte(&property) || property != NID_FILES_INFO || !ParseFilesInfo(&reader, *streams, filename, member))
    return false;
  if (!reader.ReadByte(&property) || property != NID_END)
    return false;
  return reader.IsEmpty();
}

bool DecodeNextHeader(std::span<const u8> archive, std::span<const u8> next_header, std::vector<u8>* decoded_header,
                      std::span<const u8>* header, Error* error)
{
  if (next_header.empty())
  {
    Error::SetString(error, "MAME cheat.7z has an empty header.");
    return false;
  }

  if (next_header[0] == NID_HEADER)
  {
    *header = next_header;
    return true;
  }
  if (next_header[0] != NID_ENCODED_HEADER)
  {
    Error::SetString(error, "Unsupported MAME cheat.7z header type.");
    return false;
  }

  Reader reader(next_header.subspan(1));
  StreamsInfo encoded_streams;
  if (!ParseStreamsInfo(&reader, &encoded_streams) || !reader.IsEmpty() || encoded_streams.substream_sizes.size() != 1)
  {
    Error::SetString(error, "Unsupported encoded header in MAME cheat.7z.");
    return false;
  }

  std::span<const u8> packed_header;
  if (!GetArchiveSpan(archive, encoded_streams.pack_position, encoded_streams.packed_size, &packed_header) ||
      !DecodeWholeLzma(packed_header, encoded_streams.lzma_properties, encoded_streams.unpacked_size, decoded_header, error))
  {
    return false;
  }

  if (encoded_streams.unpacked_crc.defined && ComputeCRC32(std::span<const u8>(*decoded_header)) != encoded_streams.unpacked_crc.value)
  {
    Error::SetString(error, "MAME cheat.7z decoded header failed CRC validation.");
    return false;
  }

  *header = *decoded_header;
  return true;
}

} // namespace

static SevenZipReadStatus ReadSevenZipFile(const char* archive_path, std::string_view filename, std::string* data, Error* error)
{
  Error::Clear(error);
  if (!archive_path || !data || filename.empty())
  {
    Error::SetString(error, "Invalid MAME cheat.7z read request.");
    return SevenZipReadStatus::Error;
  }
  data->clear();

  const std::optional<DynamicHeapArray<u8>> archive_data = FileSystem::ReadBinaryFile(archive_path, error);
  if (!archive_data.has_value())
    return SevenZipReadStatus::Error;
  if (archive_data->size() < START_HEADER_SIZE || archive_data->size() > MAX_ARCHIVE_SIZE)
  {
    Error::SetString(error, "Invalid MAME cheat.7z archive size.");
    return SevenZipReadStatus::Error;
  }

  const std::span<const u8> archive = archive_data->cspan();
  if (!std::equal(SEVEN_ZIP_SIGNATURE.begin(), SEVEN_ZIP_SIGNATURE.end(), archive.begin()) || archive[6] != 0)
  {
    Error::SetString(error, "Invalid MAME cheat.7z signature or version.");
    return SevenZipReadStatus::Error;
  }

  Reader start_reader(archive.subspan(8, START_HEADER_SIZE - 8));
  u32 start_header_crc;
  u64 next_header_offset;
  u64 next_header_size;
  u32 next_header_crc;
  if (!start_reader.ReadLE32(&start_header_crc) || !start_reader.ReadLE64(&next_header_offset) ||
      !start_reader.ReadLE64(&next_header_size) || !start_reader.ReadLE32(&next_header_crc) || !start_reader.IsEmpty())
  {
    Error::SetString(error, "Invalid MAME cheat.7z start header.");
    return SevenZipReadStatus::Error;
  }

  if (ComputeCRC32(archive.subspan(12, 20)) != start_header_crc)
  {
    Error::SetString(error, "MAME cheat.7z start header failed CRC validation.");
    return SevenZipReadStatus::Error;
  }

  std::span<const u8> next_header;
  if (!GetArchiveSpan(archive, next_header_offset, next_header_size, &next_header) ||
      ComputeCRC32(next_header) != next_header_crc)
  {
    Error::SetString(error, "MAME cheat.7z next header failed validation.");
    return SevenZipReadStatus::Error;
  }

  std::vector<u8> decoded_header;
  std::span<const u8> header;
  if (!DecodeNextHeader(archive, next_header, &decoded_header, &header, error))
    return SevenZipReadStatus::Error;

  StreamsInfo streams;
  MemberLocation member;
  if (!ParseDecodedHeader(header, filename, &streams, &member))
  {
    Error::SetString(error, "Unsupported or invalid MAME cheat.7z file table.");
    return SevenZipReadStatus::Error;
  }
  if (!member.found)
    return SevenZipReadStatus::NotFound;

  std::span<const u8> packed_stream;
  if (!GetArchiveSpan(archive, streams.pack_position, streams.packed_size, &packed_stream) ||
      !DecodeLzmaRange(packed_stream, streams.lzma_properties, streams.unpacked_size, member.offset, member.size, data,
                       error))
  {
    return SevenZipReadStatus::Error;
  }

  if (member.crc.defined && ComputeCRC32(std::span<const u8>(reinterpret_cast<const u8*>(data->data()), data->size())) != member.crc.value)
  {
    data->clear();
    Error::SetString(error, "MAME cheat.7z member failed CRC validation.");
    return SevenZipReadStatus::Error;
  }

  return SevenZipReadStatus::Loaded;
}

namespace {

bool IsSafeShortName(std::string_view shortname)
{
  return !shortname.empty() && shortname.find('/') == std::string_view::npos &&
         shortname.find('\\') == std::string_view::npos && shortname != "." && shortname != "..";
}

std::string GetLooseXMLPath(std::string_view cheats_directory, std::string_view shortname)
{
  return Path::Combine(cheats_directory, std::string(shortname) + ".xml");
}

std::string GetSevenZipPath(std::string_view cheats_directory)
{
  return Path::Combine(cheats_directory, "cheat.7z");
}

PackageLoadStatus TryLoadSevenZip(std::string_view cheats_directory, std::string_view shortname, bool used_parent,
                                  List* list, PackageLoadInfo* info, Error* error)
{
  const std::string archive_path = GetSevenZipPath(cheats_directory);
  if (!FileSystem::FileExists(archive_path.c_str()))
    return PackageLoadStatus::NotFound;

  const std::string member_name = std::string(shortname) + ".xml";
  std::string xml;
  const SevenZipReadStatus read_status = ReadSevenZipFile(archive_path.c_str(), member_name, &xml, error);
  if (read_status == SevenZipReadStatus::NotFound)
    return PackageLoadStatus::NotFound;
  if (read_status == SevenZipReadStatus::Error)
    return PackageLoadStatus::Error;

  const std::string source_name = archive_path + "::" + member_name;
  if (!ParseXML(source_name, xml, list, error))
    return PackageLoadStatus::Error;

  if (info)
  {
    info->source = PackageSource::SevenZip;
    info->source_path = archive_path;
    info->matched_shortname.assign(shortname);
    info->used_parent = used_parent;
  }
  return PackageLoadStatus::Loaded;
}

PackageLoadStatus TryLoadLooseXML(std::string_view cheats_directory, std::string_view shortname, bool used_parent,
                                  List* list, PackageLoadInfo* info, Error* error)
{
  const std::string path = GetLooseXMLPath(cheats_directory, shortname);
  if (!FileSystem::FileExists(path.c_str()))
    return PackageLoadStatus::NotFound;

  if (!LoadXMLFile(path.c_str(), list, error))
    return PackageLoadStatus::Error;

  if (info)
  {
    info->source = PackageSource::LooseXML;
    info->source_path = path;
    info->matched_shortname.assign(shortname);
    info->used_parent = used_parent;
  }
  return PackageLoadStatus::Loaded;
}

} // namespace

PackageLoadStatus LoadForGame(std::string_view cheats_directory, std::string_view shortname,
                              std::string_view parent_shortname, List* list, PackageLoadInfo* info, Error* error)
{
  Error::Clear(error);
  if (info)
    *info = {};

  if (!list)
  {
    Error::SetString(error, "MAME cheat output list is null.");
    return PackageLoadStatus::Error;
  }
  list->Clear();

  if (cheats_directory.empty())
  {
    Error::SetString(error, "MAME cheat directory is empty.");
    return PackageLoadStatus::Error;
  }
  if (!IsSafeShortName(shortname))
  {
    Error::SetString(error, "Invalid MAME cheat shortname.");
    return PackageLoadStatus::Error;
  }

  // Prefer an exact loose override, then the standard cheat.7z member. Parent fallback is only used
  // when neither exact source exists, matching MAME shortname package behavior.
  PackageLoadStatus status = TryLoadLooseXML(cheats_directory, shortname, false, list, info, error);
  if (status != PackageLoadStatus::NotFound)
    return status;

  status = TryLoadSevenZip(cheats_directory, shortname, false, list, info, error);
  if (status != PackageLoadStatus::NotFound)
    return status;

  if (!parent_shortname.empty() && parent_shortname != shortname)
  {
    if (!IsSafeShortName(parent_shortname))
    {
      Error::SetString(error, "Invalid MAME cheat parent shortname.");
      return PackageLoadStatus::Error;
    }

    status = TryLoadLooseXML(cheats_directory, parent_shortname, true, list, info, error);
    if (status != PackageLoadStatus::NotFound)
      return status;

    status = TryLoadSevenZip(cheats_directory, parent_shortname, true, list, info, error);
    if (status != PackageLoadStatus::NotFound)
      return status;
  }

  return PackageLoadStatus::NotFound;
}

} // namespace MAMECheats
