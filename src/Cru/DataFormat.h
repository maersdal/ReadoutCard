/// \file Cru/DataFormat.h
/// \brief Definitions of CRU data format functions
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#ifndef ALICEO2_READOUTCARD_CRU_DATAFORMAT_H_
#define ALICEO2_READOUTCARD_CRU_DATAFORMAT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Utilities/Util.h"

namespace AliceO2
{
namespace roc
{
namespace Cru
{
namespace DataFormat
{
namespace
{
  uint32_t getWord(const char* data, int i)
  {
    uint32_t word = 0;
    memcpy(&word, &data[sizeof(word)*i], sizeof(word));
    return word;
  }
} // Anonymous namespace

uint32_t getLinkId(const char* data)
{
  return Utilities::getBits(getWord(data, 2), 8, 15);
}

uint32_t getEventSize(const char* data)
{
  return Utilities::getBits(getWord(data, 3), 8, 23);
}

/// Get header size in bytes
constexpr size_t getHeaderSize()
{
  // Two 256-bit words = 64 bytes
  return 64;
}

/// Get header size in 256-bit words
constexpr size_t getHeaderSizeWords()
{
  return 2;
}

} // namespace DataFormat
} // namespace Cru
} // namespace roc
} // namespace AliceO2

#endif // ALICEO2_READOUTCARD_CRU_DATAFORMAT_H_