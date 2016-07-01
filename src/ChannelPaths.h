///
/// \file BarWrapper.h
/// \author Pascal Boeschoten
///

#pragma once

#include <boost/filesystem/path.hpp>

namespace AliceO2 {
namespace Rorc {
namespace ChannelPaths {

boost::filesystem::path pages(int serial, int channel);
boost::filesystem::path state(int serial, int channel);
boost::filesystem::path lock(int serial, int channel);
boost::filesystem::path fifo(int serial, int channel);

} // namespace ChannelPaths
} // namespace Rorc
} // namespace AliceO2