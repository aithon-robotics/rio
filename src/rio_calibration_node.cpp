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

/*
 * This node loads a rosbag with IMU and radar data, sets up a calibration
 * optimization and solves for the states and extrinsic calibration.
 */

#include <gtsam/geometry/Pose3.h>
#include <gtsam/nonlinear/ExpressionFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/NonlinearEquality.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/expressions.h>
#include <log++.h>
#include <nav_msgs/msg/odometry.hpp>
#include "rclcpp/rclcpp.hpp"
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <tf2_eigen/tf2_eigen.h>

#include "rio/common.h"
#include "rio/gtsam/expressions.h"
#include "rio/gtsam/optimization.h"

using namespace rio;
using namespace gtsam;
using namespace ros;

using gtsam::symbol_shorthand::B;
using gtsam::symbol_shorthand::C;
using gtsam::symbol_shorthand::V;
using gtsam::symbol_shorthand::X;

struct ImuMeasurement {
  double t;
  Vector3 B_a_IB;
  Vector3 B_omega_IB;
};

struct RadarDetection {
  Vector3 R_p_RT;
  double v;
};

struct RadarMeasurement {
  double t;
  std::vector<RadarDetection> detections;
};

struct OdometryMeasurement {
  double t;
  Pose3 T_IB;
  Vector3 I_v_IB;
};

int main(int argc, char** argv) {
  init(argc, argv, "rio_calibration_node");
  NodeHandle nh;
  NodeHandle nh_private("~");

  // Load parameters.
  std::string bag_path;
  if (!loadParam<std::string>(nh_private, "bag_path", &bag_path)) {
    return 1;
  }
  std::string imu_topic;
  if (!loadParam<std::string>(nh_private, "imu_topic", &imu_topic)) {
    return 1;
  }
  std::string radar_topic;
  if (!loadParam<std::string>(nh_private, "radar_topic", &radar_topic)) {
    return 1;
  }
  std::string odometry_topic;
  if (!loadParam<std::string>(nh_private, "odometry_topic", &odometry_topic)) {
    return 1;
  }

  SharedNoiseModel radar_radial_velocity_noise_model;
  if (!loadNoiseRadarRadialVelocity(nh_private,
                                    &radar_radial_velocity_noise_model)) {
    return 1;
  }

  PreintegratedCombinedMeasurements imu_integrator;
  if (!loadPreintegratedCombinedMeasurements(nh_private, &imu_integrator)) {
    return 1;
  }

  SharedNoiseModel loop_closure_noise_T;
  if (!loadNoiseLoopClosureT(nh_private, &loop_closure_noise_T)) {
    return 1;
  }

  Vector3 init_B_t_BR;
  if (!loadParam<Vector3>(nh_private, "B_t_BR", &init_B_t_BR)) return false;
  Vector4 init_q_BR;
  if (!loadParam<Vector4>(nh_private, "q_BR", &init_q_BR)) return false;
  Pose3 init_T_BR(Rot3(init_q_BR(3), init_q_BR(0), init_q_BR(1), init_q_BR(2)),
                  init_B_t_BR);
  LOG(I, "Initial calibration:");
  LOG(I, "B_t_BR [x, y, z]: " << init_T_BR.translation().transpose());
  LOG(I, "q_BR [x, y, z, w]: "
             << init_T_BR.rotation().toQuaternion().coeffs().transpose());

  // Load rosbag.
  rosbag::Bag bag;
  bag.open(bag_path, rosbag::bagmode::Read);

  // Read IMU, IMU raw and radar data from rosbag
  std::vector<OdometryMeasurement> odometry_measurements;
  std::vector<ImuMeasurement> imu_raw_measurements;
  std::vector<RadarMeasurement> radar_measurements;

  rosbag::View view(bag);
  for (const rosbag::MessageInstance& msg : view) {
    if (msg.getTopic() == odometry_topic) {
      nav_msgs::msg::Odometry::ConstSharedPtr odom_msg =
          msg.instantiate<nav_msgs::msg::Odometry>();
      Eigen::Quaterniond q_IB;
      tf2::fromMsg(odom_msg->pose.pose.orientation, q_IB);
      Vector3 I_t_IB;
      tf2::fromMsg(odom_msg->pose.pose.position, I_t_IB);
      OdometryMeasurement odometry_measurement;
      odometry_measurement.t = odom_msg->header.stamp.toSec();
      odometry_measurement.T_IB = {Rot3(q_IB), I_t_IB};
      Vector3 B_v_IB;
      tf2::fromMsg(odom_msg->twist.twist.linear, B_v_IB);
      odometry_measurement.I_v_IB = Rot3(q_IB).rotate(B_v_IB);
      odometry_measurements.push_back(odometry_measurement);
    } else if (msg.getTopic() == imu_topic) {
      sensor_msgs::msg::Imu::ConstSharedPtr imu_msg = msg.instantiate<sensor_msgs::msg::Imu>();
      ImuMeasurement imu_measurement;
      imu_measurement.t = imu_msg->header.stamp.toSec();
      tf2::fromMsg(imu_msg->linear_acceleration, imu_measurement.B_a_IB);
      tf2::fromMsg(imu_msg->angular_velocity, imu_measurement.B_omega_IB);
      imu_raw_measurements.push_back(imu_measurement);
    } else if (msg.getTopic() == radar_topic) {
      sensor_msgs::msg::PointCloud2Ptr radar_msg =
          msg.instantiate<sensor_msgs::msg::PointCloud2>();
      auto detections = parseRadarMsg(radar_msg);
      RadarMeasurement radar_measurement;
      radar_measurement.t = radar_msg->header.stamp.toSec();
      for (const auto& detection : detections) {
        RadarDetection radar_detection;
        radar_detection.R_p_RT = {detection.x, detection.y, detection.z};
        if (radar_detection.R_p_RT.norm() < 0.1) {
          LOG(W, "Ignoring radar measurement with detection distance "
                     << radar_detection.R_p_RT.norm());
          continue;
        }
        radar_detection.v = static_cast<double>(detection.velocity);
        radar_measurement.detections.push_back(radar_detection);
      }
      radar_measurements.push_back(radar_measurement);
    }
  }

  if (odometry_measurements.empty()) {
    LOG(F, "No odometry measurements found in rosbag.");
    return 1;
  }
  if (imu_raw_measurements.empty()) {
    LOG(F, "No IMU measurements found in rosbag.");
    return 1;
  }
  if (radar_measurements.empty()) {
    LOG(F, "No radar measurements found in rosbag.");
    return 1;
  }

  LOG(I, "Loaded " << odometry_measurements.size() << " odometry measurements, "
                   << imu_raw_measurements.size() << " IMU measurements and "
                   << radar_measurements.size() << " radar measurements.");

  // Filter out empty detections.
  for (auto it = radar_measurements.begin(); it != radar_measurements.end();) {
    if (it->detections.empty()) {
      it = radar_measurements.erase(it);
      LOG(I, "Removed radar measurement with no detections at time "
                 << std::setprecision(19) << it->t);
    } else {
      it++;
    }
  }

  // Filter out zero velocity at start.
  auto it_start =
      std::find_if(radar_measurements.begin(), radar_measurements.end(),
                   [](const RadarMeasurement& m) {
                     return std::any_of(
                         m.detections.begin(), m.detections.end(),
                         [](const RadarDetection& det) { return det.v != 0; });
                   });
  LOG(I, "Removing " << std::distance(radar_measurements.begin(), it_start)
                     << " radar measurements with zero velocity at start.");
  radar_measurements.erase(
      radar_measurements.begin(),
      it_start != radar_measurements.begin() ? std::prev(it_start) : it_start);
  LOG(W,
      std::any_of(radar_measurements.begin()->detections.begin(),
                  radar_measurements.begin()->detections.end(),
                  [](const RadarDetection& det) { return det.v != 0; }),
      "First radar measurement has non-zero velocity.");

  // Filter out zero velocity at end.
  auto it_end =
      std::find_if(radar_measurements.rbegin(), radar_measurements.rend(),
                   [](const RadarMeasurement& m) {
                     return std::any_of(
                         m.detections.begin(), m.detections.end(),
                         [](const RadarDetection& det) { return det.v != 0; });
                   });
  LOG(I, "Removing " << std::distance(radar_measurements.rbegin(), it_end)
                     << " radar measurements with zero velocity at end.");
  radar_measurements.erase(
      (it_end != radar_measurements.rbegin() ? std::prev(it_end) : it_end)
          .base(),
      radar_measurements.rbegin().base());
  LOG(W,
      std::any_of(radar_measurements.back().detections.begin(),
                  radar_measurements.back().detections.end(),
                  [](const RadarDetection& det) { return det.v != 0; }),
      "Last radar measurement has non-zero velocity.");

  // Find the time at which we have all measurements.
  double t_start =
      std::max({odometry_measurements.front().t, imu_raw_measurements.front().t,
                radar_measurements.front().t});
  LOG(I, "Using t_start = " << std::setprecision(19) << t_start << ".");
  // Remove measurements before t_start.
  odometry_measurements.erase(
      odometry_measurements.begin(),
      std::lower_bound(
          odometry_measurements.begin(), odometry_measurements.end(), t_start,
          [](const OdometryMeasurement& m, double t) { return m.t < t; }));
  imu_raw_measurements.erase(
      imu_raw_measurements.begin(),
      std::lower_bound(
          imu_raw_measurements.begin(), imu_raw_measurements.end(), t_start,
          [](const ImuMeasurement& m, double t) { return m.t < t; }));
  radar_measurements.erase(
      radar_measurements.begin(),
      std::lower_bound(
          radar_measurements.begin(), radar_measurements.end(), t_start,
          [](const RadarMeasurement& m, double t) { return m.t < t; }));

  // Create nonlinear factor graph.
  NonlinearFactorGraph graph;
  Values values;
  std::map<size_t, double> idx_stamp_map;

  // Add a radar factor for each radar measurement.
  size_t idx = 0;
  for (const auto& radar_measurement : radar_measurements) {
    // Initial value close to current index.
    // Could find actual closest time, but this is good enough.
    auto odom = std::lower_bound(
        odometry_measurements.begin(), odometry_measurements.end(),
        radar_measurement.t,
        [](const OdometryMeasurement& m, double t) { return m.t < t; });
    if (odom == odometry_measurements.end()) continue;
    idx_stamp_map[idx] = radar_measurement.t;
    values.insert(X(idx), odom->T_IB);
    values.insert(V(idx), odom->I_v_IB);
    values.insert(B(idx), imuBias::ConstantBias());
    values.insert(C(idx), init_T_BR);

    // Radar factor.
    auto T_IB = Pose3_(X(idx));
    auto T_BR = Pose3_(C(idx));
    // Find the next IMU message to determine angular velocity.
    auto imu = std::lower_bound(
        imu_raw_measurements.begin(), imu_raw_measurements.end(),
        radar_measurement.t,
        [](const ImuMeasurement& m, double t) { return m.t < t; });
    auto R_v_IR = unrotate(
        rotation(T_IB * T_BR),
        Vector3_(V(idx)) + rotate(rotation(T_IB),
                                  cross(correctGyroscope_(ConstantBias_(B(idx)),
                                                          imu->B_omega_IB),
                                        translation(T_BR))));
    idx++;

    for (const auto& detection : radar_measurement.detections) {
      Unit3_ R_p_TR_unit(Unit3(-detection.R_p_RT));
      auto h = radialVelocity_(R_v_IR, R_p_TR_unit);
      auto z = detection.v;
      auto factor = ExpressionFactor(radar_radial_velocity_noise_model, z, h);
      graph.add(factor);
    }
  }

  auto last_idx = std::prev(idx_stamp_map.end())->first;
  LOG(I, "Added " << last_idx + 1 << " radar factors.");

  // Add calibration between constraints.
  for (size_t i = 0; i < last_idx; i++) {
    graph.add(NonlinearEquality2<Pose3>(C(i), C(i + 1)));
  }

  // Add IMU in between factors.
  LOG(I, "Adding " << last_idx << " IMU factors.");
  for (size_t i = 0; i < last_idx; i++) {
    auto imu_begin = std::lower_bound(
        imu_raw_measurements.begin(), imu_raw_measurements.end(),
        idx_stamp_map[i],
        [](const ImuMeasurement& m, double t) { return m.t < t; });
    imu_begin->t = idx_stamp_map[i];
    auto imu_end = std::lower_bound(
        imu_begin, imu_raw_measurements.end(), idx_stamp_map[i + 1],
        [](const ImuMeasurement& m, double t) { return m.t < t; });
    imu_end->t = idx_stamp_map[i + 1];
    imu_integrator.resetIntegrationAndSetBias(
        imuBias::ConstantBias(values.at<imuBias::ConstantBias>(B(i))));
    while (imu_begin != imu_end) {
      auto dt = std::next(imu_begin)->t - imu_begin->t;
      LOG(E, dt < 0.0,
          "Negative dt: " << dt << " at time " << imu_begin->t
                          << " for IMU measurement at time "
                          << std::next(imu_begin)->t << ".");
      imu_integrator.integrateMeasurement(imu_begin->B_a_IB,
                                          imu_begin->B_omega_IB, dt);
      imu_begin++;
    }
    graph.add(CombinedImuFactor(X(i), V(i), X(i + 1), V(i + 1), B(i), B(i + 1),
                                imu_integrator));
  }

  // Add loop closure constraint.
  graph.add(
      BetweenFactor<Pose3>(X(0), X(last_idx), Pose3(), loop_closure_noise_T));

  // Add zero velocity constraint at start and end.
  graph.add(NonlinearEquality1<Vector3>(Z_3x1, V(0)));
  graph.add(NonlinearEquality1<Vector3>(Z_3x1, V(last_idx)));

  // Solve.
  LOG(I, "Solving...");
  gtsam::LevenbergMarquardtOptimizer optimizer(graph, values);
  LOG(I, "Error before optimization: " << optimizer.error());
  auto result = optimizer.optimize();
  LOG(I, "Error after optimization: " << optimizer.error());
  LOG(I, "Number of iterations: " << optimizer.iterations());

  // Print results.
  LOG(I, "Calibration results:");
  LOG(I,
      "B_t_BR [x, y, z]: " << result.at<Pose3>(C(0)).translation().transpose());
  LOG(I, "q_BR [x, y, z, w]: " << result.at<Pose3>(C(0))
                                      .rotation()
                                      .toQuaternion()
                                      .coeffs()
                                      .transpose());
  LOG(I, "IMU biases:");
  LOG(I, "B: " << result.at<imuBias::ConstantBias>(B(0)));

  // Save rosbag.
  // nav_msgs::msg::Odometry for state
  // Vector3Stamped for biases
  std::string out_bag_path =
      bag_path.substr(0, bag_path.size() - 4) + "_calibrated.bag";
  rosbag::Bag out_bag;
  out_bag.open(out_bag_path, rosbag::bagmode::Write);
  for (size_t i = 0; i <= last_idx; ++i) {
    nav_msgs::msg::Odometry odom_msg;
    odom_msg.header.stamp = rclcpp::Time(idx_stamp_map[i]);
    odom_msg.header.frame_id = "odom";
    odom_msg.child_frame_id = "bmi088";
    odom_msg.pose.pose.position =
        tf2::toMsg(result.at<Pose3>(X(i)).translation());
    odom_msg.pose.pose.orientation =
        tf2::toMsg(result.at<Pose3>(X(i)).rotation().toQuaternion());
    tf2::toMsg(
        result.at<Pose3>(X(i)).rotation().unrotate(result.at<Vector3>(V(i))),
        odom_msg.twist.twist.linear);
    out_bag.write("/rio/odometry_navigation", rclcpp::Time(idx_stamp_map[i]),
                  odom_msg);

    geometry_msgs::msg::Vector3Stamped bias_msg;
    bias_msg.header.stamp = rclcpp::Time(idx_stamp_map[i]);
    bias_msg.header.frame_id = "bmi088";
    tf2::toMsg(result.at<imuBias::ConstantBias>(B(i)).gyroscope(),
               bias_msg.vector);
    out_bag.write("/rio/bias_gyro", rclcpp::Time(idx_stamp_map[i]), bias_msg);

    tf2::toMsg(result.at<imuBias::ConstantBias>(B(i)).accelerometer(),
               bias_msg.vector);
    out_bag.write("/rio/bias_acc", rclcpp::Time(idx_stamp_map[i]), bias_msg);
  }

  return 0;
}