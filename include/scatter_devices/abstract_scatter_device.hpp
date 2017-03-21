/** \file
This file contains the interface definition for all scattering devices and
implements an abstract scattering device from which all other devices are
derived.

\author Benjamin Lindner <ben@benlabs.net>
\version 1.3.0
\copyright GNU General Public License
*/

#ifndef SCATTER_DEVICES__ABSTRACT_SCATTER_DEVICE_HPP_
#define SCATTER_DEVICES__ABSTRACT_SCATTER_DEVICE_HPP_

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
#include <boost/mpi.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <boost/thread.hpp>

// other headers
#include "decomposition/assignment.hpp"
#include "math/coor3d.hpp"
#include "report/timer.hpp"
#include "scatter_devices/scatter_factors.hpp"
#include "services.hpp"

/**
Efficent Thread-safe version of a queue
*/
template <typename Data>
class concurrent_queue {
 private:
  std::queue<Data> the_queue;
  mutable boost::mutex the_mutex;
  boost::condition_variable the_condition_variable;

 public:
  void push(Data const& data) {
    boost::mutex::scoped_lock lock(the_mutex);
    the_queue.push(data);
    lock.unlock();

    // notify everyone who tries to push
    the_condition_variable.notify_all();
  }

  bool empty() const {
    boost::mutex::scoped_lock lock(the_mutex);
    return the_queue.empty();
  }

  size_t size() const { return the_queue.size(); }

  bool try_pop(Data& popped_value) {
    boost::mutex::scoped_lock lock(the_mutex);
    if (the_queue.empty()) {
      return false;
    }

    popped_value = the_queue.front();
    the_queue.pop();
    // notify everyone who tries to push
    return true;
  }

  void wait_and_pop(Data& popped_value) {
    boost::mutex::scoped_lock lock(the_mutex);
    while (the_queue.empty()) {
      the_condition_variable.wait(lock);
    }

    popped_value = the_queue.front();
    the_queue.pop();
  }

  void wait_for_empty() {
    while (!the_queue.empty())
      boost::this_thread::sleep(boost::posix_time::milliseconds(25));
  }
};

/**
Interface class to allow for the execution of the scattering calculation
*/
class IScatterDevice {
 protected:
  virtual void runner() = 0;

  virtual size_t status() = 0;
  virtual double progress() = 0;

 public:
  virtual std::map<boost::thread::id, Timer>& getTimer() = 0;
  virtual void run() = 0;
};

/**
Abstract Scattering Device from which all others are derived. It implements
common functionality, e.g. basic control flows.
*/
class AbstractScatterDevice : public IScatterDevice {
 protected:
  coor_t* p_coordinates;

  boost::mpi::communicator allcomm_;
  boost::mpi::communicator partitioncomm_;
  Sample& sample_;

  std::vector<CartesianCoor3D> vectors_;
  size_t current_vector_;

  boost::shared_ptr<MonitorClient> p_monitor_;
  boost::shared_ptr<HDF5WriterClient> p_hdf5writer_;

  size_t NN, NF, NA;

  fftw_complex* atfinal_;
  std::complex<double> afinal_;
  std::complex<double> a2final_;

  ScatterFactors scatterfactors;

  virtual void stage_data() = 0;
  virtual void compute() = 0;

  void next();
  void write();

  void runner();

  virtual void print_pre_stage_info() {}
  virtual void print_post_stage_info() {}
  virtual void print_pre_runner_info() {}
  virtual void print_post_runner_info() {}

  virtual bool ram_check();
  void start_workers();
  void stop_workers();
  virtual void worker() = 0;
  std::queue<boost::thread*> worker_threads;
  boost::barrier* workerbarrier;

  size_t status();
  double progress();

  std::map<boost::thread::id, Timer> timer_;

 public:
  std::map<boost::thread::id, Timer>& getTimer();

  AbstractScatterDevice(boost::mpi::communicator allcomm,
                        boost::mpi::communicator partitioncomm, Sample& sample,
                        std::vector<CartesianCoor3D> vectors, size_t NAF,
                        boost::asio::ip::tcp::endpoint fileservice_endpoint,
                        boost::asio::ip::tcp::endpoint monitorservice_endpoint);
  ~AbstractScatterDevice();

  void run();
};

#endif

// end of file
