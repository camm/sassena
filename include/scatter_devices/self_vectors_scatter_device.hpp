/** \file
This file contains a class which implements the scattering calculation for self
scattering.

\author Benjamin Lindner <ben@benlabs.net>
\version 1.3.0
\copyright GNU General Public License
*/

#ifndef SCATTER_DEVICES__SELF_VECTORS_SCATTER_DEVICE_HPP_
#define SCATTER_DEVICES__SELF_VECTORS_SCATTER_DEVICE_HPP_

// common header
#include "common.hpp"

// standard header
#include <sys/time.h>
#include <complex>
#include <map>
#include <queue>
#include <string>
#include <vector>

// special library headers
#include <fftw3.h>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/asio.hpp>
#include <boost/mpi.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <boost/thread.hpp>

// other headers
#include "math/coor3d.hpp"
#include "report/timer.hpp"
#include "sample.hpp"

#include "scatter_devices/abstract_vectors_scatter_device.hpp"

/**
Implements self type scattering using vectors for orientational averaging
*/
class SelfVectorsScatterDevice : public AbstractVectorsScatterDevice {
 protected:
  fftw_complex* at_;
  // first = q, second = frames
  concurrent_queue<std::pair<size_t, size_t> > atscatter_;
  coor_t* p_coordinates;
  size_t current_atomindex_;

  //////////////////////////////
  // methods
  //////////////////////////////

  fftw_complex* scatter(size_t qindex, size_t aindex);

  ModAssignment assignment_;

  double progress();

  void stage_data();

  void worker();
  void compute();

  void scatterblock(size_t atomindex, size_t index, size_t count);
  void store(fftw_complex* at);
  void dsp(fftw_complex* at);

  bool ram_check();

  ~SelfVectorsScatterDevice();
  fftw_plan fftw_planF_;
  fftw_plan fftw_planB_;

 public:
  SelfVectorsScatterDevice(
      boost::mpi::communicator allcomm, boost::mpi::communicator partitioncomm,
      Sample& sample, std::vector<CartesianCoor3D> vectors, size_t NAF,
      boost::asio::ip::tcp::endpoint fileservice_endpoint,
      boost::asio::ip::tcp::endpoint monitorservice_endpoint);
};

#endif