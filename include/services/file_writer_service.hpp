/** \file
This file implements a service for writing results asynchronously to the signal
output file and manages the buffered communication between clients and the file
server (head node).

\author Benjamin Lindner <ben@benlabs.net>
\version 1.3.0
\copyright GNU General Public License
*/

#ifndef IO__FILE_WRITER_SERVICE_HPP_
#define IO__FILE_WRITER_SERVICE_HPP_

// common header
#include "common.hpp"

// standard header
#include <complex>
#include <map>
#include <queue>
#include <string>
#include <vector>

// special library headers
#include <fftw3.h>
#include <hdf5.h>
#include <hdf5_hl.h>
#include <boost/asio.hpp>
#include <boost/date_time.hpp>
#include <boost/thread.hpp>

// other headers
#include "math/coor3d.hpp"

/**
Defines communication tags between client/server
*/
enum HDF5WriterTag { HANGUP, WRITE };

/**
Type class which is used to buffer the result data before it is written to file
*/
struct HDF5DataEntry {
  CartesianCoor3D qvector;
  std::vector<std::complex<double> >* p_fqt;
  std::complex<double> fq0;
  std::complex<double> fq;
  std::complex<double> fq2;
};

/**
Client side code which implements buffered writing of the results and does
automatic flushing of the data towards the server
*/
class HDF5WriterClient {
  boost::asio::ip::tcp::endpoint m_endpoint;
  boost::posix_time::ptime m_lastflush;

  std::queue<HDF5DataEntry> data_queue;

 public:
  HDF5WriterClient(boost::asio::ip::tcp::endpoint server);
  ~HDF5WriterClient();

  void write(CartesianCoor3D qvector,
             const std::vector<std::complex<double> >& data,
             const std::complex<double> data2,
             const std::complex<double> data3);
  void write(CartesianCoor3D qvector, const fftw_complex* data, size_t NF,
             const std::complex<double> data2,
             const std::complex<double> data3);

  void flush();
};

/**
Server side code which implements buffered writing of the results and does
automatic flushing of the data into the signal file
*/
class HDF5WriterService {
  std::string m_filename;
  boost::posix_time::ptime m_lastflush;

  boost::asio::io_service& m_io_service;
  boost::asio::ip::tcp::acceptor m_acceptor;

  boost::thread* m_listener;
  bool m_listener_status;

  //    std::vector<size_t> init(const std::vector<CartesianCoor3D>&
  //    qvectors,size_t nf);
  //    std::vector<size_t> init_new(const std::vector<CartesianCoor3D>&
  //    qvectors,size_t nf);
  //    std::vector<size_t> init_reuse(const std::vector<CartesianCoor3D>&
  //    qvectors,size_t nf);
  void init_new(size_t nf);
  void init(size_t nf);

  bool test_fqt_dim(size_t nf);

  void listener();

  void flush();

  std::vector<CartesianCoor3D> data_qvectors;
  std::vector<std::vector<std::complex<double> >*> data_fqt;
  std::vector<std::complex<double> > data_fq0;
  std::vector<std::complex<double> > data_fq;
  std::vector<std::complex<double> > data_fq2;

 public:
  HDF5WriterService(boost::asio::io_service& io_service,
                    const std::string filename, size_t nf);

  boost::asio::ip::tcp::endpoint get_endpoint() {
    return m_acceptor.local_endpoint();
  }
  std::vector<CartesianCoor3D> get_qvectors();

  void hangup();
  void run();
};

#endif
