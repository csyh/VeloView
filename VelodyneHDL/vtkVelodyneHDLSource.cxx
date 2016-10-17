// Copyright 2013 Velodyne Acoustics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkVelodyneHDLSource.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkVelodyneHDLSource.h"
#include "vtkVelodyneHDLReader.h"
#include "vtkPacketFileReader.h"
#include "vtkPacketFileWriter.h"
#include "vtkPolyData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkSmartPointer.h"
#include "vtkNew.h"
#include "vtkTransform.h"

#include <boost/thread/thread.hpp>
#include <boost/asio.hpp>

#include <queue>
#include <deque>

//----------------------------------------------------------------------------
namespace
{

  template<typename T>
  class SynchronizedQueue
  {
    public:

      SynchronizedQueue () :
        queue_(), mutex_(), cond_(), request_to_end_(false), enqueue_data_(true) { }

      void
      enqueue (const T& data)
      {
        boost::unique_lock<boost::mutex> lock (mutex_);

        if (enqueue_data_)
        {
          queue_.push (data);
          cond_.notify_one ();
        }
      }

      bool
      dequeue (T& result)
      {
        boost::unique_lock<boost::mutex> lock (mutex_);

        while (queue_.empty () && (!request_to_end_))
        {
          cond_.wait (lock);
        }

        if (request_to_end_)
        {
          doEndActions ();
          return false;
        }

        result = queue_.front ();
        queue_.pop ();

        return true;
      }

      void
      stopQueue ()
      {
        boost::unique_lock<boost::mutex> lock (mutex_);
        request_to_end_ = true;
        cond_.notify_one ();
      }

      unsigned int
      size ()
      {
        boost::unique_lock<boost::mutex> lock (mutex_);
        return static_cast<unsigned int> (queue_.size ());
      }

      bool
      isEmpty () const
      {
        boost::unique_lock<boost::mutex> lock (mutex_);
        return (queue_.empty ());
      }

    private:
      void
      doEndActions ()
      {
        enqueue_data_ = false;

        while (!queue_.empty ())
        {
          queue_.pop ();
        }
      }

      std::queue<T> queue_;              // Use STL queue to store data
      mutable boost::mutex mutex_;       // The mutex to synchronise on
      boost::condition_variable cond_;   // The condition to wait for

      bool request_to_end_;
      bool enqueue_data_;
  };

//----------------------------------------------------------------------------
class PacketConsumer
{
public:

  PacketConsumer()
  {
    this->NewData = false;
    this->MaxNumberOfDatasets = 1000;
    this->LastTime = 0.0;
  }

  void HandleSensorData(const unsigned char* data, unsigned int length)
  {
    boost::lock_guard<boost::mutex> lock(this->ReaderMutex);

    // Accumulate HDL6 Status byte data
    if(this->HDLReader->getIsHDL64Data()
       && !this->HDLReader->getCorrectionsInitialized())
      {
        this->HDLReader->appendRollingDataAndTryCorrection(data);
      }
    else
    {
      this->HDLReader->ProcessHDLPacket(const_cast<unsigned char*>(data), length);
      if (this->HDLReader->GetDatasets().size())
      {
      this->HandleNewData(this->HDLReader->GetDatasets().back());
      this->HDLReader->GetDatasets().clear();
      }
    }

  }

  vtkSmartPointer<vtkPolyData> GetDatasetForTime(double timeRequest, double& actualTime)
  {
    boost::lock_guard<boost::mutex> lock(this->ConsumerMutex);

    size_t stepIndex = this->GetIndexForTime(timeRequest);
    if (stepIndex < this->Timesteps.size())
      {
      actualTime = this->Timesteps[stepIndex];
      return this->Datasets[stepIndex];
      }
    actualTime = 0;
    return 0;
  }

  std::vector<double> GetTimesteps()
  {
    boost::lock_guard<boost::mutex> lock(this->ConsumerMutex);
    const size_t nTimesteps = this->Timesteps.size();
    std::vector<double> timesteps(nTimesteps, 0);
    for (size_t i = 0; i < nTimesteps; ++i)
      {
      timesteps[i] = this->Timesteps[i];
      }
    return timesteps;
  }


  int GetMaxNumberOfDatasets()
  {
    return this->MaxNumberOfDatasets;
  }

  void SetMaxNumberOfDatasets(int nDatasets)
  {
    boost::lock_guard<boost::mutex> lock(this->ConsumerMutex);
    this->MaxNumberOfDatasets = nDatasets;
    this->UpdateDequeSize();
  }

  bool CheckForNewData()
  {
    boost::lock_guard<boost::mutex> lock(this->ConsumerMutex);
    bool newData = this->NewData;
    this->NewData = false;
    return newData;
  }


  void ThreadLoop()
  {
    std::string* packet = 0;
    while (this->Packets->dequeue(packet))
      {
      this->HandleSensorData(reinterpret_cast<const unsigned char*>(packet->c_str()), packet->length());
      delete packet;
      }
  }

  void Start()
  {
    if (this->Thread)
      {
      return;
      }

    this->Packets.reset(new SynchronizedQueue<std::string*>);
    this->Thread = boost::shared_ptr<boost::thread>(
      new boost::thread(boost::bind(&PacketConsumer::ThreadLoop, this)));
  }

  void Stop()
  {
    if (this->Thread)
      {
      this->Packets->stopQueue();
      this->Thread->join();
      this->Thread.reset();
      this->Packets.reset();
      }
  }

  void Enqueue(std::string* packet)
  {
    this->Packets->enqueue(packet);
  }

  vtkVelodyneHDLReader* GetReader()
  {
    return this->HDLReader.GetPointer();
  }

  // Hold this when running reader code code or modifying its internals
  boost::mutex ReaderMutex;

protected:

  void UpdateDequeSize()
  {
    if (this->MaxNumberOfDatasets <= 0)
      {
      return;
      }
    while (this->Datasets.size() >= this->MaxNumberOfDatasets)
      {
      this->Datasets.pop_front();
      this->Timesteps.pop_front();
      }
  }

  size_t GetIndexForTime(double time)
  {
    size_t index = 0;
    double minDifference = VTK_DOUBLE_MAX;
    const size_t nTimesteps = this->Timesteps.size();
    for (size_t i = 0; i < nTimesteps; ++i)
      {
      double difference = std::abs(this->Timesteps[i] - time);
        if (difference < minDifference)
          {
          minDifference = difference;
          index = i;
          }
      }
    return index;
  }

  void HandleNewData(vtkSmartPointer<vtkPolyData> polyData)
  {
    boost::lock_guard<boost::mutex> lock(this->ConsumerMutex);

    this->UpdateDequeSize();
    this->Timesteps.push_back(this->LastTime);
    this->Datasets.push_back(polyData);
    this->NewData = true;
    this->LastTime += 1.0;
  }

  bool NewData;
  int MaxNumberOfDatasets;
  double LastTime;

  // Hold this when modifying internals of reader
  boost::mutex ConsumerMutex;

  std::deque<vtkSmartPointer<vtkPolyData> > Datasets;
  std::deque<double> Timesteps;
  vtkNew<vtkVelodyneHDLReader> HDLReader;

  boost::shared_ptr<SynchronizedQueue<std::string*> > Packets;

  boost::shared_ptr<boost::thread> Thread;
};


//----------------------------------------------------------------------------
class PacketFileWriter
{
public:

  void ThreadLoop()
  {
    std::string* packet = 0;
    while (this->Packets->dequeue(packet))
      {
      this->PacketWriter.WritePacket(reinterpret_cast<const unsigned char*>(packet->c_str()), packet->length());

      delete packet;
      }
  }

  void Start(const std::string& filename)
  {
    if (this->Thread)
      {
      return;
      }

    if (this->PacketWriter.GetFileName() != filename)
      {
      this->PacketWriter.Close();
      }

    if (!this->PacketWriter.IsOpen())
      {
      if (!this->PacketWriter.Open(filename))
        {
        vtkGenericWarningMacro("Failed to open packet file: " << filename);
        return;
        }
      }

    this->Packets.reset(new SynchronizedQueue<std::string*>);
    this->Thread = boost::shared_ptr<boost::thread>(
      new boost::thread(boost::bind(&PacketFileWriter::ThreadLoop, this)));
  }

  void Stop()
  {
    if (this->Thread)
      {
      this->Packets->stopQueue();
      this->Thread->join();
      this->Thread.reset();
      this->Packets.reset();
      }
  }

  void Enqueue(std::string* packet)
  {
    this->Packets->enqueue(packet);
  }

  bool IsOpen()
  {
    return this->PacketWriter.IsOpen();
  }

  void Close()
  {
    this->PacketWriter.Close();
  }

private:
  vtkPacketFileWriter PacketWriter;
  boost::shared_ptr<boost::thread> Thread;
  boost::shared_ptr<SynchronizedQueue<std::string*> > Packets;
};

class PacketNetworkSource;
//----------------------------------------------------------------------------
/**
* \class PacketReceiver
* \brief Defines the protocol used to receive the packet on the network
* Here it is used to setup an UDP multicast protocol. The packets come from
* the Velodyne sensor and it exists two differents kind : Position packet which contains 
* information about the GPS and : Data packet which contains the information about the sensors.
* @param io The in/out service used to handle the reception of the packets
* @param port The port address which will receive the packet
* @param forwardport The port adress which will receive the forwarded packets
* @param forwarddestinationIp The IP adress of the computer which will receive the forwarded packets
* @param flagforward  Allow or not the forwarding of the packets
* @param parent  the PacketNetworkSource inherit parent
*/
class PacketReceiver
{
public:
  PacketReceiver(boost::asio::io_service& io, int port,int forwardport, std::string forwarddestinationIp, bool flagforward, PacketNetworkSource* parent)
  : Port(port),
    PacketCounter(0),
    isForwarding(flagforward),
    ForwardedPort(forwardport),
    destinationIp(forwarddestinationIp),
    ForwardEndpoint(boost::asio::ip::address_v4::from_string(forwarddestinationIp), forwardport),
    Socket(io),
    ForwardedSocket(io),
    Parent(parent),
    IsReceiving(true),
    ShouldStop(false)
  {
	    Socket.open(boost::asio::ip::udp::v4()); //Opening the socket with an UDP v4 protocol
	    Socket.set_option(boost::asio::ip::udp::socket::reuse_address(true)); //Tell the OS we accept to re-use the port address for an other app
	    Socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)); //Bind the socket to the right address


     if(isForwarding)
      {
      ForwardedSocket.open(ForwardEndpoint.protocol()); //Opening the socket with an UDP v4 protocol toward the forwarded ip address and port
      }

    this->StartReceive();
  }

  ~PacketReceiver()
  {
    this->Socket.cancel();
    if(isForwarding)
      {
      this->ForwardedSocket.cancel();
      }
      {
      boost::unique_lock<boost::mutex> guard(this->IsReceivingMtx);
      this->ShouldStop = true;
      while(this->IsReceiving)
        {
        this->IsReceivingCond.wait(guard);
        }
      }
  }

  void StartReceive()
  {
      {
      boost::lock_guard<boost::mutex> guard(this->IsReceivingMtx);
      this->IsReceiving = true;
      }

    // expecting exactly 1206 bytes, using a larger buffer so that if a
    // larger packet arrives unexpectedly we'll notice it.
    this->Socket.async_receive(boost::asio::buffer(this->RXBuffer, 1500),
      boost::bind(&PacketReceiver::SocketCallback, this,
      boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
  }

  void SocketCallback(const boost::system::error_code& error, std::size_t numberOfBytes);

private:
  bool isForwarding; /*!< Allow or not the forwarding of the packets */
  boost::asio::ip::udp::endpoint ForwardEndpoint;
  int Port; /*!< Port address which will receive the packet */
  int ForwardedPort; /*!< Port address which will receive the forwarded packet */
  vtkIdType PacketCounter; /*!< Number of packets received */
  std::string destinationIp;
  boost::asio::ip::udp::socket Socket; /*!< Socket : determines the protocol used and the address used for the reception of the packets */
  boost::asio::ip::udp::socket ForwardedSocket; /*!< Socket : determines the protocol used and the address used for the reception of the forwarded packets */
  PacketNetworkSource* Parent;
  char RXBuffer[1500];  /*!< Buffer which will saved the data. Expecting exactly 1206 bytes, using a larger buffer so that 
						if a larger packet arrives unexpectedly we'll notice it. */
  bool IsReceiving; /*!< Flag indicating if the socket is receiving packets */
  bool ShouldStop; /*!< Flag indicating if we should stop the listening */
  boost::mutex IsReceivingMtx; /*!< Mutex : Block the access of IsReceiving when a thread is seting the flag */
  boost::condition_variable IsReceivingCond;
};

//----------------------------------------------------------------------------
// 
/**
* \class PacketReceiver
* \brief This class is responsible for the IOService and  two PacketReceiver classes
* @param _consumer boost::shared_ptr<PacketConsumer>
* @param argLIDARPort The used port to receive the LIDAR information 
* @param argPositionPort The used port to receive the GPS information 
* @param ForwardedLIDARPort_ The port which will receive the lidar forwarded packets
* @param ForwardedGPSPort_ The port which will receive the gps forwarded packets
* @param ForwardedIpAddress_ The ip which will receive the forwarded packets
* @param isForwarding_ Allow the forwarding
*/
class PacketNetworkSource
{
public:
  PacketNetworkSource(boost::shared_ptr<PacketConsumer> _consumer, 
                      int argLIDARPort, int argGPSPort, 
                      int ForwardedLIDARPort_, 
                      int ForwardedGPSPort_, 
                      std::string ForwardedIpAddress_, 
                      bool isForwarding_) :
    IOService(),
    Thread(),
    LIDARPortReceiver(),
    PositionPortReceiver(),
    Consumer(_consumer),
    Writer(),
    DummyWork(new boost::asio::io_service::work(this->IOService)),
    LIDARPort(argLIDARPort),
    GPSPort(argGPSPort),
    ForwardedLIDARPort(ForwardedLIDARPort_),
    ForwardedGPSPort(ForwardedGPSPort_),
    ForwardedIpAddress(ForwardedIpAddress_),
    isForwarding(isForwarding_)
  {
  }

  ~PacketNetworkSource()
  {
    this->Stop();

    delete this->DummyWork;

    if(this->Thread)
      {
      this->Thread->join();
      }
  }

  void QueuePackets(std::string* packet)
  {
    std::string* packet2 = 0;
    if(this->Writer)
      {
      packet2 = new std::string(*packet);
      }

    if( this->Consumer )
      {
      this->Consumer->Enqueue(packet);
      }

    if (this->Writer)
      {
      this->Writer->Enqueue(packet2);
      }
  }

  void Start()
  {
    if(!this->Thread)
      {
      std::cout << "Start listen" << std::endl;
      this->Thread.reset(new boost::thread(boost::bind(&boost::asio::io_service::run, &this->IOService)));
      }

    if(this->LIDARPortReceiver)
      {
      assert(this->PositionPortReceiver);
      return;
      }

    // Create work
    this->LIDARPortReceiver = boost::shared_ptr<PacketReceiver>(new PacketReceiver(this->IOService, LIDARPort,ForwardedLIDARPort,ForwardedIpAddress,isForwarding, this));
    this->PositionPortReceiver = boost::shared_ptr<PacketReceiver>(new PacketReceiver(this->IOService, GPSPort,ForwardedGPSPort,ForwardedIpAddress,isForwarding, this));
  }

  void Stop()
  {
    // Kill the receivers
    this->PositionPortReceiver.reset();
    this->LIDARPortReceiver.reset();

  }
  bool isForwarding; /*!< Allowing the forward of the packets */
  std::string ForwardedIpAddress; /*!< Ip of the computer which will receive the forwarded packets */
  int ForwardedLIDARPort; /*!< Port address which will receive the lidar forwarded packet */
  int ForwardedGPSPort; /*!< Port address which will receive the Gps forwarded packet */
  int LIDARPort; /*!< Listening port for LIDAR information */
  int GPSPort;   /*!< Listening port for GPS information */
  boost::asio::io_service IOService; /*!< The in/out service which will handle the Packets */
  boost::shared_ptr<boost::thread> Thread;

  boost::shared_ptr<PacketReceiver> LIDARPortReceiver; /*!< The PacketReceiver configured to receive LIDAR information */
  boost::shared_ptr<PacketReceiver> PositionPortReceiver; /*!< The PacketReceiver configured to receive GPS information */

  boost::shared_ptr<PacketConsumer> Consumer;
  boost::shared_ptr<PacketFileWriter> Writer;

  boost::asio::io_service::work* DummyWork;
};

void PacketReceiver::SocketCallback(const boost::system::error_code& error, std::size_t numberOfBytes)
{
//  std::cout << "CALLBACK " << this->Port << std::endl;
  if(error || this->ShouldStop)
    {
    // This is called on cancel
    // TODO: Check other error codes
      {
      boost::lock_guard<boost::mutex> guard(this->IsReceivingMtx);
        this->IsReceiving = false;
      }
      this->IsReceivingCond.notify_one();

    return;
    }

  std::string* packet = new std::string(this->RXBuffer, numberOfBytes);
 

  if(isForwarding)
    {
      size_t bytesSent = ForwardedSocket.send_to(boost::asio::buffer( packet->c_str(), numberOfBytes),
                                                              ForwardEndpoint);
    }

  this->Parent->QueuePackets(packet);


  this->StartReceive();

  if ((++this->PacketCounter % 500) == 0)
    {
    std::cout << "RECV packets: " << this->PacketCounter << " on " << this->Port << std::endl;;
    }
}

} // end namespace

//----------------------------------------------------------------------------
/**
* \class vtkVelodyneHDLSource::vtkInternal
* \brief This class is responsible for Consumer, the Writer and the NetWorkSource classes
*/
class vtkVelodyneHDLSource::vtkInternal
{
public:
  /**
* \function vtkVelodyneHDLSource::vtkInternal
* \brief Constructor allowing customizable listening port 
* @param argLIDARPort The used port to receive the lidar data
* @param argPositionPort The used port to receive the GPS data
*/
  vtkInternal( int argLIDARPort,
               int argGPSPort,
               int ForwardedLIDARPort_, 
               int ForwardedGPSPort_, 
               std::string ForwardedIpAddress_, 
               bool isForwarding_) : Consumer(new PacketConsumer),
                                      Writer(new PacketFileWriter),
                                      NetworkSource(this->Consumer,argLIDARPort,argGPSPort,ForwardedLIDARPort_, ForwardedGPSPort_,  ForwardedIpAddress_, isForwarding_)
  {
  }

  ~vtkInternal()
  {
  }

  boost::shared_ptr<PacketConsumer> Consumer;
  boost::shared_ptr<PacketFileWriter> Writer;
  PacketNetworkSource NetworkSource;
};

//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkVelodyneHDLSource);

//----------------------------------------------------------------------------
vtkVelodyneHDLSource::vtkVelodyneHDLSource()
{
  this->LIDARPort = 2368; //The default used port
  this->GPSPort = 8308;   //The default used port
  this->isForwarding = false;
  this->Internal = new vtkInternal(LIDARPort,GPSPort,ForwardedLIDARPort, ForwardedGPSPort, ForwardedIpAddress, isForwarding); 
 
  this->SetNumberOfInputPorts(0);
  this->SetNumberOfOutputPorts(1);
}

//----------------------------------------------------------------------------
vtkVelodyneHDLSource::~vtkVelodyneHDLSource()
{
  this->Stop();
  delete this->Internal;
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLSource::GetOutputFile()
{
  return this->OutputFile;
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetOutputFile(const std::string& filename)
{
  if (filename == this->GetOutputFile())
    {
    return;
    }

  this->Internal->Writer->Close();
  this->OutputFile = filename;
  this->Modified();
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLSource::GetNumberOfChannels()
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  return this->Internal->Consumer->GetReader()->GetNumberOfChannels();
}

//-----------------------------------------------------------------------------
const std::string& vtkVelodyneHDLSource::GetCorrectionsFile()
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  return this->Internal->Consumer->GetReader()->GetCorrectionsFile();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCorrectionsFile(const std::string& filename)
{
  if (filename == this->GetCorrectionsFile())
    {
    return;
    }

  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetCorrectionsFile(filename);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetLaserSelection(int LaserSelection[64])
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetLaserSelection(LaserSelection);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::GetLaserSelection(int LaserSelection[64])
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->GetLaserSelection(LaserSelection);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCropReturns(int cr)
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetCropReturns(cr);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCropInside(int ci)
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetCropInside(ci);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCropRegion(double r[6])
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetCropRegion(r);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCropRegion(double xmin, double xmax,
                                         double ymin, double ymax,
                                         double zmin, double zmax)
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetCropRegion(xmin, xmax,
                                                       ymin, ymax,
                                                       zmin, zmax);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::GetLaserCorrections(
      double verticalCorrection[64],
      double rotationalCorrection[64],
      double distanceCorrection[64],
      double distanceCorrectionX[64],
      double distanceCorrectionY[64],
      double verticalOffsetCorrection[64],
      double horizontalOffsetCorrection[64],
      double focalDistance[64],
      double focalSlope[64],
      double minIntensity[64],
      double maxIntensity[64])
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->GetLaserCorrections(
       verticalCorrection,
       rotationalCorrection,
       distanceCorrection,
       distanceCorrectionX,
       distanceCorrectionY,
       verticalOffsetCorrection,
       horizontalOffsetCorrection,
       focalDistance,
       focalSlope,
       minIntensity,
       maxIntensity);
  this->Modified();
}
//-----------------------------------------------------------------------------
unsigned int vtkVelodyneHDLSource::GetDualReturnFilter() const
{
  return this->Internal->Consumer->GetReader()->GetDualReturnFilter();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetDualReturnFilter(unsigned int filter)
{
  this->Internal->Consumer->GetReader()->SetDualReturnFilter(filter);
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetDummyProperty(int vtkNotUsed(dummy))
{
  this->Modified();
}

//-----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetSensorTransform(vtkTransform* transform)
{
  boost::lock_guard<boost::mutex> lock(this->Internal->Consumer->ReaderMutex);

  this->Internal->Consumer->GetReader()->SetSensorTransform(transform);
  this->Modified();
}

//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::Start()
{
  if (this->OutputFile.length())
    {
    this->Internal->Writer->Start(this->OutputFile);
    }

  this->Internal->NetworkSource.Writer.reset();

  if (this->Internal->Writer->IsOpen())
    {
    this->Internal->NetworkSource.Writer = this->Internal->Writer;
    }

  this->Internal->Consumer->Start();
  this->Internal->NetworkSource.LIDARPort = this->LIDARPort;
  this->Internal->NetworkSource.GPSPort = this->GPSPort;
  this->Internal->NetworkSource.ForwardedGPSPort = this->ForwardedGPSPort;
  this->Internal->NetworkSource.ForwardedLIDARPort = this->ForwardedLIDARPort;
  this->Internal->NetworkSource.ForwardedIpAddress = this->ForwardedIpAddress;
  this->Internal->NetworkSource.isForwarding = this->isForwarding;
  this->Internal->NetworkSource.Start();
}

//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::Stop()
{
  this->Internal->NetworkSource.Stop();
  this->Internal->Consumer->Stop();
  this->Internal->Writer->Stop();
}

//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::ReadNextFrame()
{
}


//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::Poll()
{
  if (this->Internal->Consumer->CheckForNewData())
    {
    this->Modified();
    }
}

//----------------------------------------------------------------------------
int vtkVelodyneHDLSource::GetCacheSize()
{
  return this->Internal->Consumer->GetMaxNumberOfDatasets();
}

//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::SetCacheSize(int cacheSize)
{
  if (cacheSize == this->GetCacheSize())
    {
    return;
    }

  this->Internal->Consumer->SetMaxNumberOfDatasets(cacheSize);
  this->Modified();
}

//-----------------------------------------------------------------------------
int vtkVelodyneHDLSource::RequestInformation(vtkInformation *request,
                                     vtkInformationVector **inputVector,
                                     vtkInformationVector *outputVector)
{
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  std::vector<double> timesteps = this->Internal->Consumer->GetTimesteps();
  const size_t nTimesteps = timesteps.size();
  if (nTimesteps > 0)
    {
    outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), &timesteps.front(), static_cast<int>(nTimesteps));
    }
  else
    {
    outInfo->Remove(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    }

  double timeRange[2] = {0.0, nTimesteps ? nTimesteps - 1.0 : 0.0};
  outInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), timeRange, 2);

  return 1;
}

//----------------------------------------------------------------------------
int vtkVelodyneHDLSource::RequestData(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *outputVector)
{
  vtkInformation *outInfo = outputVector->GetInformationObject(0);
  vtkDataSet *output = vtkDataSet::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  double timeRequest = 0;
  if (outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP()))
    {
    timeRequest = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP());
    }


  double actualTime;
  vtkSmartPointer<vtkPolyData> polyData = this->Internal->Consumer->GetDatasetForTime(timeRequest, actualTime);
  if (polyData)
    {
    //printf("request %f, returning %f\n", timeRequest, actualTime);
    output->GetInformation()->Set(vtkDataObject::DATA_TIME_STEP(), actualTime);
    output->ShallowCopy(polyData);
    }

  return 1;
}

//----------------------------------------------------------------------------
void vtkVelodyneHDLSource::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}
