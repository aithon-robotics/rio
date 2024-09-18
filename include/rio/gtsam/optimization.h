/*
BSD 3-Clause License

Copyright (c) 2024 ETH Zurich, Autonomous Systems Lab, Rik Girod

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include <gtsam/nonlinear/FixedLagSmoother.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam_unstable/nonlinear/IncrementalFixedLagSmoother.h>

#include <rio/msg/timing.hpp>
#include "rio/gtsam/propagation.h"

namespace rio {

class Optimization {
 public:
  Optimization(){};
  bool solve(const std::deque<Propagation>& propagations);
  bool getResult(std::deque<Propagation>* propagation,
                 std::map<std::string, Timing>* timing);

  void addPriorFactor(const Propagation& propagation,
                      const gtsam::SharedNoiseModel& noise_model_I_T_IB,
                      const gtsam::SharedNoiseModel& noise_model_I_v_IB,
                      const gtsam::SharedNoiseModel& noise_model_imu_bias);
  void addRadarFactor(const Propagation& propagation_to_radar,
                      const Propagation& propagation_from_radar,
                      const gtsam::SharedNoiseModel& noise_model_radar_doppler,
                      const gtsam::SharedNoiseModel& noise_model_radar_track,
                      std::vector<gtsam::Vector1>* doppler_residuals = nullptr);
  void addBaroFactor(const Propagation& propagation_to_baro,
                     const gtsam::SharedNoiseModel& noise_model_baro_height,
                     gtsam::Vector1* baro_residual = nullptr);

  inline void setSmoother(const gtsam::IncrementalFixedLagSmoother& smoother) {
    smoother_ = smoother;
  }

 private:
  void solveThreaded(const gtsam::NonlinearFactorGraph graph,
                     const gtsam::Values values,
                     const gtsam::FixedLagSmoother::KeyTimestampMap stamps,
                     std::deque<Propagation> propagations);

  template <typename T>
  void addFactor(const Propagation& propagation,
                 const gtsam::SharedNoiseModel& noise_model = nullptr);

  void addDopplerFactors(
      const Propagation& propagation,
      const gtsam::SharedNoiseModel& noise_model = nullptr,
      std::vector<gtsam::Vector1>* doppler_residuals = nullptr);

  void updateTiming(
      const std::shared_ptr<const ::gtsam::internal::TimingOutline>& variable,
      const std::string& label, const rclcpp::Time& stamp);

  gtsam::NonlinearFactorGraph new_graph_;
  gtsam::Values new_values_;
  gtsam::FixedLagSmoother::KeyTimestampMap new_timestamps_;

  // Variables that should not be accessed while thread is running.
  // Mutex lock!
  std::map<std::string, Timing> timing_;
  bool new_result_{false};
  std::deque<Propagation> propagations_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;

  // The smoother must not be changed while the thread is running.
  gtsam::IncrementalFixedLagSmoother smoother_;
};

}  // namespace rio