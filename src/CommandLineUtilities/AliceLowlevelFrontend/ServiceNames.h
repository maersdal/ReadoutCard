/// \file ServiceNames.h
/// \brief Definition of ALICE Lowlevel Frontend (ALF) DIM service names
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#ifndef ALICEO2_READOUTCARD_UTILITIES_ALF_ALICELOWLEVELFRONTEND_SERVICENAMES_H
#define ALICEO2_READOUTCARD_UTILITIES_ALF_ALICELOWLEVELFRONTEND_SERVICENAMES_H

#include <string>

namespace AliceO2 {
namespace roc {
namespace CommandLineUtilities {
namespace Alf {

class ServiceNames
{
  public:
    ServiceNames(int serialNumber, int link)
        : serial(serialNumber), link(link)
    {
    }

    std::string registerReadRpc() const;
    std::string registerWriteRpc() const;
    std::string scaWrite() const;
    std::string scaSequence() const;
    std::string scaRead() const;
    std::string scaGpioWrite() const;
    std::string scaGpioRead() const;
    std::string temperature() const;
    std::string publishRegistersStart() const;
    std::string publishRegistersStop() const;
    std::string publishScaSequenceStart() const;
    std::string publishScaSequenceStop() const;
    std::string publishRegistersSubdir(std::string name) const;
    std::string publishScaSequenceSubdir(std::string name) const;

  private:
    std::string format(std::string name) const;
    const int serial;
    const int link;
};

} // namespace Alf
} // namespace CommandLineUtilities
} // namespace roc
} // namespace AliceO2

#endif // ALICEO2_READOUTCARD_UTILITIES_ALF_ALICELOWLEVELFRONTEND_SERVICENAMES_H
