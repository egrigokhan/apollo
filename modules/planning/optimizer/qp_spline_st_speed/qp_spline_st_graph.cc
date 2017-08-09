/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file qp_spline_st_graph.cc
 **/

#include "modules/planning/optimizer/qp_spline_st_speed/qp_spline_st_graph.h"

#include <algorithm>
#include <string>
#include <utility>

#include "modules/common/log.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleParam;

QpSplineStGraph::QpSplineStGraph(
    const QpSplineStSpeedConfig& qp_spline_st_speed_config,
    const VehicleParam& veh_param)
    : qp_spline_st_speed_config_(qp_spline_st_speed_config),
      t_knots_resolution_(
          qp_spline_st_speed_config_.total_time() /
          qp_spline_st_speed_config_.number_of_discrete_graph_t()),
      t_evaluated_resolution_(
          qp_spline_st_speed_config_.total_time() /
          qp_spline_st_speed_config_.number_of_evaluated_graph_t())

{}

void QpSplineStGraph::Init() {
  // init knots
  double curr_t = 0.0;
  for (uint32_t i = 0;
       i <= qp_spline_st_speed_config_.number_of_discrete_graph_t(); ++i) {
    t_knots_.push_back(curr_t);
    curr_t += t_knots_resolution_;
  }

  // init evaluated t positions
  curr_t = 0.0;
  for (uint32_t i = 0;
       i <= qp_spline_st_speed_config_.number_of_evaluated_graph_t(); ++i) {
    t_evaluated_.push_back(curr_t);
    curr_t += t_evaluated_resolution_;
  }

  // init spline generator
  spline_generator_.reset(new Spline1dGenerator(
      t_knots_, qp_spline_st_speed_config_.spline_order()));
}

Status QpSplineStGraph::Search(const StGraphData& st_graph_data,
                               const PathData& path_data,
                               SpeedData* const speed_data) {
  init_point_ = st_graph_data.init_point();
  if (st_graph_data.path_data_length() <
      qp_spline_st_speed_config_.total_path_length()) {
    qp_spline_st_speed_config_.set_total_path_length(
        st_graph_data.path_data_length());
  }

  // TODO(all): update config through veh physical limit here generate knots

  // initialize time resolution and
  Init();

  if (!ApplyConstraint(st_graph_data.init_point(), st_graph_data.speed_limit(),
                       st_graph_data.st_graph_boundaries())
           .ok()) {
    const std::string msg = "Apply constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  if (!ApplyKernel(st_graph_data.st_graph_boundaries(),
                   st_graph_data.speed_limit())
           .ok()) {
    const std::string msg = "Apply kernel failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  if (!Solve().ok()) {
    const std::string msg = "Solve qp problem failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // extract output
  speed_data->Clear();
  const Spline1d& spline = spline_generator_->spline();

  double t_output_resolution =
      qp_spline_st_speed_config_.output_time_resolution();
  double time = 0.0;
  while (time < qp_spline_st_speed_config_.total_time() + t_output_resolution) {
    double s = spline(time);
    double v = spline.Derivative(time);
    double a = spline.SecondOrderDerivative(time);
    double da = spline.ThirdOrderDerivative(time);
    speed_data->add_speed_point(s, time, v, a, da);
    time += t_output_resolution;
  }

  return Status::OK();
}

Status QpSplineStGraph::ApplyConstraint(
    const common::TrajectoryPoint& init_point, const SpeedLimit& speed_limit,
    const std::vector<StGraphBoundary>& boundaries) {
  Spline1dConstraint* constraint =
      spline_generator_->mutable_spline_constraint();
  // position, velocity, acceleration

  if (!constraint->AddPointConstraint(0.0, 0.0)) {
    const std::string msg = "add st start point constraint failed";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }
  ADEBUG << "init point constraint:" << init_point.DebugString();
  if (!constraint->AddPointDerivativeConstraint(0.0, init_point_.v())) {
    const std::string msg = "add st start point velocity constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  if (!constraint->AddPointSecondDerivativeConstraint(0.0,
                                                          init_point_.a())) {
    const std::string msg =
        "add st start point acceleration constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  if (!constraint->AddPointSecondDerivativeConstraint(
          spline_generator_->spline().x_knots().back(), 0.0)) {
    const std::string msg = "add st end point acceleration constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // monotone constraint
  if (!constraint->AddMonotoneInequalityConstraintAtKnots()) {
    const std::string msg = "add monotonicity constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // smoothness constraint
  if (!constraint->AddThirdDerivativeSmoothConstraint()) {
    const std::string msg = "add smoothness joint constraint failed!";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  // boundary constraint
  std::vector<double> s_upper_bound;
  std::vector<double> s_lower_bound;

  for (const double curr_t : t_evaluated_) {
    double lower_s = 0.0;
    double upper_s = 0.0;
    GetSConstraintByTime(boundaries, curr_t,
                         qp_spline_st_speed_config_.total_path_length(),
                         &upper_s, &lower_s);
    s_upper_bound.push_back(upper_s);
    s_lower_bound.push_back(lower_s);
    ADEBUG << "Add constraint by time: " << curr_t << " upper_s: " << upper_s
        << " lower_s: " << lower_s;
  }
  if (!constraint->AddBoundary(t_evaluated_, s_lower_bound,
                                   s_upper_bound)) {
    const std::string msg = "Fail to apply distance constraints.";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  std::vector<double> speed_upper_bound;
  if (!EstimateSpeedUpperBound(init_point, speed_limit, &speed_upper_bound)
           .ok()) {
    std::string msg = "Fail to estimate speed upper constraints.";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  std::vector<double> speed_lower_bound(t_evaluated_.size(), 0.0);
  if (!constraint->AddDerivativeBoundary(t_evaluated_, speed_lower_bound,
                                           speed_upper_bound)) {
    const std::string msg = "Fail to apply speed constraints.";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }

  return Status::OK();
}

Status QpSplineStGraph::ApplyKernel(
    const std::vector<StGraphBoundary>& boundaries,
    const SpeedLimit& speed_limit) {
  Spline1dKernel* spline_kernel = spline_generator_->mutable_spline_kernel();

  if (qp_spline_st_speed_config_.speed_kernel_weight() > 0) {
    spline_kernel->add_derivative_kernel_matrix(
        qp_spline_st_speed_config_.speed_kernel_weight());
  }

  if (qp_spline_st_speed_config_.accel_kernel_weight() > 0) {
    spline_kernel->add_second_order_derivative_matrix(
        qp_spline_st_speed_config_.accel_kernel_weight());
  }

  if (qp_spline_st_speed_config_.jerk_kernel_weight() > 0) {
    spline_kernel->add_third_order_derivative_matrix(
        qp_spline_st_speed_config_.jerk_kernel_weight());
  }

  if (AddCruiseReferenceLineKernel(t_evaluated_, speed_limit) != Status::OK()) {
    return Status(ErrorCode::PLANNING_ERROR, "QpSplineStGraph::ApplyKernel");
  }

  if (AddFollowReferenceLineKernel(t_evaluated_, boundaries, 1.0) !=
      Status::OK()) {
    return Status(ErrorCode::PLANNING_ERROR, "QpSplineStGraph::ApplyKernel");
  }
  return Status::OK();
}

Status QpSplineStGraph::Solve() {
  return spline_generator_->Solve()
             ? Status::OK()
             : Status(ErrorCode::PLANNING_ERROR, "QpSplineStGraph::solve");
}

Status QpSplineStGraph::AddCruiseReferenceLineKernel(
    const std::vector<double>& evaluate_t, const SpeedLimit& speed_limit) {
  auto* spline_kernel = spline_generator_->mutable_spline_kernel();
  std::vector<double> s_vec;
  if (speed_limit.speed_points().size() == 0) {
    std::string msg = "Fail to apply_kernel due to empty speed limits.";
    AERROR << msg;
    return Status(ErrorCode::PLANNING_ERROR, msg);
  }
  double dist_ref = 0.0;
  for (uint32_t i = 1; i < evaluate_t.size(); ++i) {
    s_vec.push_back(dist_ref);
    dist_ref += (evaluate_t[i] - evaluate_t[i - 1]) *
                speed_limit.get_speed_limit_by_s(dist_ref);
  }
  spline_kernel->add_reference_line_kernel_matrix(
      evaluate_t, s_vec,
      qp_spline_st_speed_config_.reference_line_kernel_weight());

  return Status::OK();
}

Status QpSplineStGraph::AddFollowReferenceLineKernel(
    const std::vector<double>& evaluate_t,
    const std::vector<StGraphBoundary>& boundaries, const double weight) {
  auto* spline_kernel = spline_generator_->mutable_spline_kernel();
  std::vector<double> ref_s;
  std::vector<double> filtered_evaluate_t;
  for (const double curr_t : evaluate_t) {
    double s_upper = 0.0;
    double s_lower = 0.0;
    double s_ref_min = std::numeric_limits<double>::infinity();
    bool success = false;
    for (const auto& boundary : boundaries) {
      if (boundary.boundary_type() == StGraphBoundary::BoundaryType::FOLLOW &&
          boundary.GetUnblockSRange(curr_t, &s_upper, &s_lower)) {
        success = true;
        s_ref_min =
            std::min(s_ref_min, s_upper - boundary.characteristic_length());
      }
    }
    if (success) {
      filtered_evaluate_t.push_back(curr_t);
      ref_s.push_back(s_ref_min);
    }
  }
  spline_kernel->add_reference_line_kernel_matrix(filtered_evaluate_t, ref_s,
                                                  weight);
  return Status::OK();
}

Status QpSplineStGraph::GetSConstraintByTime(
    const std::vector<StGraphBoundary>& boundaries, const double time,
    const double total_path_s, double* const s_upper_bound,
    double* const s_lower_bound) const {
  *s_upper_bound = total_path_s;

  for (const StGraphBoundary& boundary : boundaries) {
    double s_upper = 0.0;
    double s_lower = 0.0;

    if (!boundary.GetUnblockSRange(time, &s_upper, &s_lower)) {
      continue;
    }

    if (boundary.boundary_type() == StGraphBoundary::BoundaryType::STOP ||
        boundary.boundary_type() == StGraphBoundary::BoundaryType::FOLLOW ||
        boundary.boundary_type() == StGraphBoundary::BoundaryType::YIELD) {
      *s_upper_bound = std::fmin(*s_upper_bound, s_upper);
    } else {
      *s_lower_bound = std::fmax(*s_lower_bound, s_lower);
    }
  }

  return Status::OK();
}

Status QpSplineStGraph::EstimateSpeedUpperBound(
    const common::TrajectoryPoint& init_point, const SpeedLimit& speed_limit,
    std::vector<double>* speed_upper_bound) const {
  DCHECK_NOTNULL(speed_upper_bound);
  speed_upper_bound->clear();

  // use v to estimate position: not accurate, but feasible in cyclic
  // processing. We can do the following process multiple times and use
  // previous cycle's results for better estimation.
  const double v = init_point.v();

  uint32_t i = 0;
  uint32_t j = 0;
  double distance = 0.0;
  const double kDistanceEpsilon = 1e-6;
  while (i < t_evaluated_.size() && j + 1 < speed_limit.speed_points().size()) {
    distance = v * t_evaluated_[i];
    if (fabs(distance - speed_limit.speed_points()[j].s()) < kDistanceEpsilon) {
      speed_upper_bound->push_back(speed_limit.speed_points()[j].v());
      ++i;
    } else if (speed_limit.speed_points()[j + 1].s() < distance) {
      ++j;
    } else {
      speed_upper_bound->push_back(speed_limit.get_speed_limit_by_s(distance));
      ++i;
    }
  }

  for (uint32_t k = speed_upper_bound->size() - 1; k < t_evaluated_.size();
       ++k) {
    speed_upper_bound->push_back(qp_spline_st_speed_config_.max_speed());
  }
  return Status::OK();
}

}  // namespace planning
}  // namespace apollo
