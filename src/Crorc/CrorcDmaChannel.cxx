/// \file CrorcDmaChannel.cxx
/// \brief Implementation of the CrorcDmaChannel class.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#include "CrorcDmaChannel.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <thread>
#include <boost/circular_buffer.hpp>
#include <boost/format.hpp>
#include "ChannelPaths.h"
#include "Crorc/Constants.h"
#include "Utilities/SmartPointer.h"

namespace b = boost;
namespace bip = boost::interprocess;
namespace bfs = boost::filesystem;
using std::cout;
using std::endl;
using namespace std::literals;

namespace AliceO2 {
namespace roc {

CrorcDmaChannel::CrorcDmaChannel(const Parameters& parameters)
    : DmaChannelPdaBase(parameters, allowedChannels()), //
    mPdaBar(getRocPciDevice().getPciDevice(), getChannelNumber()), // Initialize main DMA channel BAR
    mPdaBar2(getRocPciDevice().getPciDevice(), 2), // Initialize BAR 2
    mPageSize(parameters.getDmaPageSize().get_value_or(8*1024)), // 8 kB default for uniformity with CRU
    mInitialResetLevel(ResetLevel::Internal), // It's good to reset at least the card channel in general
    mNoRDYRX(true), // Not sure
    mUseFeeAddress(false), // Not sure
    mLoopbackMode(parameters.getGeneratorLoopback().get_value_or(LoopbackMode::Internal)), // Internal loopback by default
    mGeneratorEnabled(parameters.getGeneratorEnabled().get_value_or(true)), // Use data generator by default
    mGeneratorPattern(parameters.getGeneratorPattern().get_value_or(GeneratorPattern::Incremental)), //
    mGeneratorMaximumEvents(0), // Infinite events
    mGeneratorInitialValue(0), // Start from 0
    mGeneratorInitialWord(0), // First word
    mGeneratorSeed(mGeneratorPattern == GeneratorPattern::Random ? 1 : 0), // We use a seed for random only
    mGeneratorDataSize(parameters.getGeneratorDataSize().get_value_or(mPageSize)), // Can use page size
    mUseContinuousReadout(parameters.getReadoutMode().is_initialized() ?
            parameters.getReadoutModeRequired() == ReadoutMode::Continuous : false)
{
  // Create and register our ReadyFIFO buffer
  log("Initializing ReadyFIFO DMA buffer", InfoLogger::InfoLogger::Debug);
  {
    // Create and register the buffer
    // Note: if resizing the file fails, we might've accidentally put the file in a hugetlbfs mount with 1 GB page size
    constexpr auto FIFO_SIZE = sizeof(ReadyFifo);
    Utilities::resetSmartPtr(mBufferFifoFile, getPaths().fifo(), FIFO_SIZE, true);
    Utilities::resetSmartPtr(mPdaDmaBufferFifo, getRocPciDevice().getPciDevice(), mBufferFifoFile->getAddress(),
        FIFO_SIZE, getPdaDmaBufferIndexFifo(getChannelNumber()), false);// note the 'false' at the end specifies non-hugepage memory

    const auto& entry = mPdaDmaBufferFifo->getScatterGatherList().at(0);
    if (entry.size < FIFO_SIZE) {
      // Something must've failed at some point
      BOOST_THROW_EXCEPTION(Exception()
          << ErrorInfo::Message("Scatter gather list entry for internal FIFO was too small")
          << ErrorInfo::ScatterGatherEntrySize(entry.size)
          << ErrorInfo::FifoSize(FIFO_SIZE));
    }
    mReadyFifoAddressUser = entry.addressUser;
    mReadyFifoAddressBus = entry.addressBus;
  }

  getReadyFifoUser()->reset();
  mDmaBufferUserspace = getBufferProvider().getAddress();
}

auto CrorcDmaChannel::allowedChannels() -> AllowedChannels {
  return {0, 1, 2, 3, 4, 5};
}

CrorcDmaChannel::~CrorcDmaChannel()
{
  deviceStopDma();
}

void CrorcDmaChannel::deviceStartDma()
{
  // With the C-RORC, we can't start DMA until we have enough memory to cover 128 DMA pages (which should be covered by
  // 1 superpage). So we set the "pending DMA start" state and actually start once a superpage has been pushed.
  log("DMA start deferred until superpage available");

  mFifoBack = 0;
  mFifoSize = 0;
  mSuperpageQueue.clear();
  mPendingDmaStart = true;
}

void CrorcDmaChannel::startPendingDma(SuperpageQueueEntry& entry)
{
  if (!mPendingDmaStart) {
    return;
  }

  log("Starting pending DMA");

  if (mUseContinuousReadout) {
    log("Initializing continuous readout");
    Crorc::Crorc::initReadoutContinuous(mPdaBar2);
  }

  // Find DIU version, required for armDdl()
  mDiuConfig = getCrorc().initDiuVersion();

  // Resetting the card,according to the RESET LEVEL parameter
  deviceResetChannel(mInitialResetLevel);

  // Setting the card to be able to receive data
  startDataReceiving();

  // Initializing the firmware FIFO, pushing (entries) pages
  for(int i = 0; i < READYFIFO_ENTRIES; ++i){
    getReadyFifoUser()->entries[i].reset();
    pushIntoSuperpage(entry);
  }

  assert(entry.pushedPages <= entry.maxPages);
  if (entry.pushedPages == entry.maxPages) {
    // Remove superpage from pushing queue
    mSuperpageQueue.removeFromPushingQueue();
  }

  if (mGeneratorEnabled) {
    log("Starting data generator");
    // Starting the data generator
    startDataGenerator();
  } else {
    if (!mNoRDYRX) {
      log("Starting trigger");

      // Clearing SIU/DIU status.
      getCrorc().assertLinkUp();
      getCrorc().siuCommand(Ddl::RandCIFST);
      getCrorc().diuCommand(Ddl::RandCIFST);

      // RDYRX command to FEE
      getCrorc().startTrigger(mDiuConfig);
    }
  }

  /// Fixed wait for initial pages TODO polling wait with timeout
  std::this_thread::sleep_for(10ms);
  if (dataArrived(READYFIFO_ENTRIES - 1) != DataArrivalStatus::WholeArrived) {
    log("Initial pages not arrived", InfoLogger::InfoLogger::Warning);
  }

  entry.superpage.received += READYFIFO_ENTRIES * mPageSize;

  if (entry.superpage.getReceived() == entry.superpage.getSize()) {
    entry.superpage.ready = true;
    mSuperpageQueue.moveFromArrivalsToFilledQueue();
  }

  getReadyFifoUser()->reset();
  mFifoBack = 0;
  mFifoSize = 0;

  mPendingDmaStart = false;
  log("DMA started");

  if (mUseContinuousReadout) {
    log("Starting continuous readout");
    Crorc::Crorc::startReadoutContinuous(mPdaBar2);
  }
}

void CrorcDmaChannel::deviceStopDma()
{
  if (mGeneratorEnabled) {
    // Starting the data generator
    startDataGenerator();
    getCrorc().stopDataGenerator();
    getCrorc().stopDataReceiver();
  } else {
    if (!mNoRDYRX) {
      // Sending EOBTR to FEE.
      getCrorc().stopTrigger(mDiuConfig);
    }
  }
}

void CrorcDmaChannel::deviceResetChannel(ResetLevel::type resetLevel)
{
  if (resetLevel == ResetLevel::Nothing) {
    return;
  }

  try {
    if (resetLevel == ResetLevel::Internal) {
      getCrorc().resetCommand(Rorc::Reset::FF, mDiuConfig);
      getCrorc().resetCommand(Rorc::Reset::RORC, mDiuConfig);
    }

    if (LoopbackMode::isExternal(mLoopbackMode)) {
      getCrorc().armDdl(Rorc::Reset::DIU, mDiuConfig);

      if ((resetLevel == ResetLevel::InternalDiuSiu) && (mLoopbackMode != LoopbackMode::Diu))
      {
        // Wait a little before SIU reset.
        std::this_thread::sleep_for(100ms); /// XXX Why???
        // Reset SIU.
        getCrorc().armDdl(Rorc::Reset::SIU, mDiuConfig);
        getCrorc().armDdl(Rorc::Reset::DIU, mDiuConfig);
      }

      getCrorc().armDdl(Rorc::Reset::RORC, mDiuConfig);
    }
  }
  catch (Exception& e) {
    e << ErrorInfo::ResetLevel(resetLevel);
    e << ErrorInfo::LoopbackMode(mLoopbackMode);
    throw;
  }

  // Wait a little after reset.
  std::this_thread::sleep_for(100ms); /// XXX Why???
}

void CrorcDmaChannel::startDataGenerator()
{
  if (LoopbackMode::None == mLoopbackMode) {
    getCrorc().startTrigger(mDiuConfig);
  }

  getCrorc().armDataGenerator(mGeneratorInitialValue, mGeneratorInitialWord, mGeneratorPattern, mGeneratorDataSize,
      mGeneratorSeed);

  if (LoopbackMode::Internal == mLoopbackMode) {
    getCrorc().setLoopbackOn();
    std::this_thread::sleep_for(100ms); // XXX Why???
  }

  if (LoopbackMode::Siu == mLoopbackMode) {
    getCrorc().setSiuLoopback(mDiuConfig);
    std::this_thread::sleep_for(100ms); // XXX Why???
    getCrorc().assertLinkUp();
    getCrorc().siuCommand(Ddl::RandCIFST);
    getCrorc().diuCommand(Ddl::RandCIFST);
  }

  getCrorc().startDataGenerator(mGeneratorMaximumEvents);
}

void CrorcDmaChannel::startDataReceiving()
{
  getCrorc().initDiuVersion();

  // Preparing the card.
  if (LoopbackMode::Siu == mLoopbackMode) {
    deviceResetChannel(ResetLevel::InternalDiuSiu);
    getCrorc().assertLinkUp();
    getCrorc().siuCommand(Ddl::RandCIFST);
    getCrorc().diuCommand(Ddl::RandCIFST);
  }

  getCrorc().resetCommand(Rorc::Reset::FF, mDiuConfig);
  std::this_thread::sleep_for(10ms); /// XXX Give card some time to reset the FreeFIFO
  getCrorc().assertFreeFifoEmpty();
  getCrorc().startDataReceiver(mReadyFifoAddressBus);
}

int CrorcDmaChannel::getTransferQueueAvailable()
{
  return mSuperpageQueue.getQueueAvailable();
}

int CrorcDmaChannel::getReadyQueueSize()
{
  return mSuperpageQueue.getFilled().size();
}

auto CrorcDmaChannel::getSuperpage() -> Superpage
{
  return mSuperpageQueue.getFrontSuperpage();
}

void CrorcDmaChannel::pushSuperpage(Superpage superpage)
{
  checkSuperpage(superpage);
  constexpr size_t MIN_SIZE = 1*1024*1024;

  if (!Utilities::isMultiple(superpage.getSize(), MIN_SIZE)) {
    BOOST_THROW_EXCEPTION(CrorcException()
        << ErrorInfo::Message("Could not enqueue superpage, C-RORC backend requires superpage size multiple of 1 MiB"));
    // We require 1 MiB because this fits 128 8KiB DMA pages (see deviceStartDma() for why we need that)
  }

  SuperpageQueueEntry entry;
  entry.busAddress = getBusOffsetAddress(superpage.getOffset());
  entry.maxPages = superpage.getSize() / mPageSize;
  entry.pushedPages = 0;
  entry.superpage = superpage;
  entry.superpage.received = 0;

  mSuperpageQueue.addToQueue(entry);
}

auto CrorcDmaChannel::popSuperpage() -> Superpage
{
  return mSuperpageQueue.removeFromFilledQueue().superpage;
}

void CrorcDmaChannel::fillSuperpages()
{
  // Push new pages into superpage
  if (!mSuperpageQueue.getPushing().empty()) {
    SuperpageQueueEntry& entry = mSuperpageQueue.getPushingFrontEntry();

    if (mPendingDmaStart) {
      // Do some special handling of first transfers......
      startPendingDma(entry);
    } else {
      int freeDescriptors = FIFO_QUEUE_MAX - mFifoSize;
      int freePages = entry.getUnpushedPages();
      int possibleToPush = std::min(freeDescriptors, freePages);

      for (int i = 0; i < possibleToPush; ++i) {
        pushIntoSuperpage(entry);
      }

      if (entry.isPushed()) {
        // Remove superpage from pushing queue
        mSuperpageQueue.removeFromPushingQueue();
      }
    }
  }

  // Check for arrivals & handle them
  if (!mSuperpageQueue.getArrivals().empty()) {
    auto isArrived = [&](int descriptorIndex) {return dataArrived(descriptorIndex) == DataArrivalStatus::WholeArrived;};
    auto resetDescriptor = [&](int descriptorIndex) {getReadyFifoUser()->entries[descriptorIndex].reset();};

    while (mFifoSize > 0) {
      SuperpageQueueEntry& entry = mSuperpageQueue.getArrivalsFrontEntry();

      if (isArrived(mFifoBack)) {
        // XXX Dirty hack for now: write length field into page SDH. In upcoming firmwares, the card will do this
        // itself
        auto writeSdhEventSize = [](uintptr_t pageAddress, uint32_t eventSize){
          constexpr size_t OFFSET_SDH_EVENT_SIZE = 16; // 1 * 128b word
//          auto address = reinterpret_cast<char*>(pageAddress + OFFSET_SDH_EVENT_SIZE);
//          // Clear first 3 32b values of event size word
//          memset(address, 0, sizeof(uint32_t) * 3);
//          // Write to 4th 32b value of event size word
//          memcpy(address + (sizeof(uint32_t) * 3), &eventSize, sizeof(uint32_t));
          auto address = reinterpret_cast<volatile uint32_t*>(pageAddress + OFFSET_SDH_EVENT_SIZE);
          address[0] = 0;
          address[1] = 0;
          address[2] = 0;
          address[3] = eventSize;
        };

        uint32_t length = getReadyFifoUser()->entries[mFifoBack].getSize();
        auto pageAddress = mDmaBufferUserspace + entry.superpage.getOffset() + entry.superpage.received;
        writeSdhEventSize(pageAddress, length);

        resetDescriptor(mFifoBack);
        mFifoSize--;
        mFifoBack = (mFifoBack + 1) % READYFIFO_ENTRIES;
        entry.superpage.received += mPageSize;

        if (entry.superpage.isFilled()) {
          // Move superpage to filled queue
          entry.superpage.ready = true;
          mSuperpageQueue.moveFromArrivalsToFilledQueue();
        }
      } else {
        // If the back one hasn't arrived yet, the next ones will certainly not have arrived either...
        break;
      }
    }
  }
}

void CrorcDmaChannel::pushIntoSuperpage(SuperpageQueueEntry& entry)
{
  assert(mFifoSize < FIFO_QUEUE_MAX);
  assert(entry.pushedPages < entry.maxPages);

  pushFreeFifoPage(getFifoFront(), getNextSuperpageBusAddress(entry));
  mFifoSize++;
  entry.pushedPages++;
}

uintptr_t CrorcDmaChannel::getNextSuperpageBusAddress(const SuperpageQueueEntry& entry)
{
  auto offset = mPageSize * entry.pushedPages;
  uintptr_t pageBusAddress = entry.busAddress + offset;
  return pageBusAddress;
}

void CrorcDmaChannel::pushFreeFifoPage(int readyFifoIndex, uintptr_t pageBusAddress)
{
  size_t pageWords = mPageSize / 4; // Size in 32-bit words
  getCrorc().pushRxFreeFifo(pageBusAddress, pageWords, readyFifoIndex);
}

CrorcDmaChannel::DataArrivalStatus::type CrorcDmaChannel::dataArrived(int index)
{
  auto length = getReadyFifoUser()->entries[index].length;
  auto status = getReadyFifoUser()->entries[index].status;

  if (status == -1) {
    return DataArrivalStatus::NoneArrived;
  } else if (status == 0) {
    return DataArrivalStatus::PartArrived;
  } else if ((status & 0xff) == Ddl::DTSW) {
    // Note: when internal loopback is used, the length of the event in words is also stored in the status word.
    // For example, the status word could be 0x400082 for events of size 4 kiB
    if ((status & (1 << 31)) != 0) {
      // The error bit is set
      BOOST_THROW_EXCEPTION(CrorcDataArrivalException()
          << ErrorInfo::Message("Data arrival status word contains error bits")
          << ErrorInfo::ReadyFifoStatus(status)
          << ErrorInfo::ReadyFifoLength(length)
          << ErrorInfo::FifoIndex(index));
    }
    return DataArrivalStatus::WholeArrived;
  }

  BOOST_THROW_EXCEPTION(CrorcDataArrivalException()
      << ErrorInfo::Message("Unrecognized data arrival status word")
      << ErrorInfo::ReadyFifoStatus(status)
      << ErrorInfo::ReadyFifoLength(length)
      << ErrorInfo::FifoIndex(index));
}

CardType::type CrorcDmaChannel::getCardType()
{
  return CardType::Crorc;
}

boost::optional<int32_t> CrorcDmaChannel::getSerial()
{
  return Crorc::getSerial(mPdaBar);
}

boost::optional<std::string> CrorcDmaChannel::getFirmwareInfo()
{
  uint32_t version = mPdaBar.readRegister(Rorc::RFID);
  auto bits = [&](int lsb, int msb) { return Utilities::getBits(version, lsb, msb); };

  uint32_t reserved = bits(24, 31);
  uint32_t major = bits(20, 23);
  uint32_t minor = bits(13, 19);
  uint32_t year = bits(9, 12) + 2000;
  uint32_t month = bits(5, 8);
  uint32_t day = bits(0, 4);

  if (reserved != 0x2) {
    BOOST_THROW_EXCEPTION(CrorcException()
        << ErrorInfo::Message("Static field of version register did not equal 0x2"));
  }

  std::ostringstream stream;
  stream << major << '.' << minor << ':' << year << '-' << month << '-' << day;
  return stream.str();
}

} // namespace roc
} // namespace AliceO2

