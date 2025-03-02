// Copyright 2024 Polaris Xia
// Copyright 2025 Lihan Chen
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

#include "pb_nav2_plugins/behaviors/back_up_free_space.hpp"

#include <cmath>

namespace pb_nav2_behaviors
{

void BackUpFreeSpace::onConfigure()
{
  auto node = node_.lock();
  if (!node) {
    throw std::runtime_error{"Failed to lock node"};
  }

  nav2_util::declare_parameter_if_not_declared(node, "global_frame", rclcpp::ParameterValue("map"));
  nav2_util::declare_parameter_if_not_declared(node, "robot_radius", rclcpp::ParameterValue(0.1));
  nav2_util::declare_parameter_if_not_declared(node, "max_radius", rclcpp::ParameterValue(1.0));
  nav2_util::declare_parameter_if_not_declared(
    node, "service_name", rclcpp::ParameterValue(std::string("local_costmap/get_costmap")));
  nav2_util::declare_parameter_if_not_declared(node, "free_threshold", rclcpp::ParameterValue(5));
  nav2_util::declare_parameter_if_not_declared(node, "visualize", rclcpp::ParameterValue(false));

  node->get_parameter("global_frame", global_frame_);
  node->get_parameter("robot_radius", robot_radius_);
  node->get_parameter("max_radius", max_radius_);
  node->get_parameter("service_name", service_name_);
  node->get_parameter("free_threshold", free_threshold_);
  node->get_parameter("visualize", visualize_);

  if (max_radius_ < robot_radius_) {
    RCLCPP_WARN(logger_, "max_radius < robot_radius. Adjusting max_radius.");
    max_radius_ = robot_radius_;
  }

  costmap_client_ = node->create_client<nav2_msgs::srv::GetCostmap>(service_name_);

  if (visualize_) {
    marker_pub_ = node->template create_publisher<visualization_msgs::msg::MarkerArray>(
      "back_up_free_space_markers", 1);
    marker_pub_->on_activate();

    marker_pub_line_ = node->template create_publisher<visualization_msgs::msg::MarkerArray>(
      "back_up_free_space_line", 1);
    marker_pub_line_->on_activate();
  }
}

void BackUpFreeSpace::onCleanup()
{
  costmap_client_.reset();
  marker_pub_.reset();
  marker_pub_line_.reset();
}

nav2_behaviors::Status BackUpFreeSpace::onRun(
  const std::shared_ptr<const BackUpAction::Goal> command)
{
  while (!costmap_client_->wait_for_service(std::chrono::seconds(1))) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(logger_, "Interrupted while waiting for the service. Exiting.");
      return nav2_behaviors::Status::FAILED;
    }
    RCLCPP_WARN(logger_, "service not available, waiting again...");
  }

  auto request = std::make_shared<nav2_msgs::srv::GetCostmap::Request>();
  auto result = costmap_client_->async_send_request(request);
  if (result.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
    RCLCPP_ERROR(logger_, "Interrupted while waiting for the service. Exiting.");
    return nav2_behaviors::Status::FAILED;
  }

  // get costmap
  auto costmap = result.get()->map;

  if (!nav2_util::getCurrentPose(
        initial_pose_, *tf_, global_frame_, robot_base_frame_, transform_tolerance_)) {
    RCLCPP_ERROR(logger_, "Initial robot pose is not available.");
    return nav2_behaviors::Status::FAILED;
  }

  // get current pose
  geometry_msgs::msg::Pose2D pose;
  pose.x = initial_pose_.pose.position.x;
  pose.y = initial_pose_.pose.position.y;
  pose.theta = tf2::getYaw(initial_pose_.pose.orientation);

  // Find the best direction to back up
  float best_angle = findBestDirection(costmap, pose, -M_PI, M_PI, max_radius_, M_PI / 32.0);

  // Calculate move command
  twist_x_ = std::cos(best_angle) * command->speed;
  twist_y_ = std::sin(best_angle) * command->speed;
  command_x_ = command->target.x;
  command_time_allowance_ = command->time_allowance;

  end_time_ = clock_->now() + command_time_allowance_;

  if (!nav2_util::getCurrentPose(
        initial_pose_, *tf_, global_frame_, robot_base_frame_, transform_tolerance_)) {
    RCLCPP_ERROR(logger_, "Initial robot pose is not available.");
    return nav2_behaviors::Status::FAILED;
  }
  RCLCPP_WARN(
    logger_, "backing up %f meters towards free space at angle %f", command_x_, best_angle);

  if (visualize_) {
    geometry_msgs::msg::Point target_point;
    target_point.x = initial_pose_.pose.position.x + command_x_ * std::cos(best_angle);
    target_point.y = initial_pose_.pose.position.y + command_x_ * std::sin(best_angle);
    visualize(target_point);
  }

  return nav2_behaviors::Status::SUCCEEDED;
}

nav2_behaviors::Status BackUpFreeSpace::onCycleUpdate()
{
  rclcpp::Duration time_remaining = end_time_ - clock_->now();
  if (time_remaining.seconds() < 0.0 && command_time_allowance_.seconds() > 0.0) {
    stopRobot();
    RCLCPP_WARN(
      logger_,
      "Exceeded time allowance before reaching the "
      "DriveOnHeading goal - Exiting DriveOnHeading");
    return nav2_behaviors::Status::FAILED;
  }

  geometry_msgs::msg::PoseStamped current_pose;
  if (!nav2_util::getCurrentPose(
        current_pose, *tf_, global_frame_, robot_base_frame_, transform_tolerance_)) {
    RCLCPP_ERROR(logger_, "Current robot pose is not available.");
    return nav2_behaviors::Status::FAILED;
  }

  float diff_x = initial_pose_.pose.position.x - current_pose.pose.position.x;
  float diff_y = initial_pose_.pose.position.y - current_pose.pose.position.y;
  float distance = hypot(diff_x, diff_y);

  feedback_->distance_traveled = distance;
  action_server_->publish_feedback(feedback_);

  if (distance >= std::fabs(command_x_)) {
    stopRobot();
    return nav2_behaviors::Status::SUCCEEDED;
  }

  auto cmd_vel = std::make_unique<geometry_msgs::msg::Twist>();
  cmd_vel->linear.y = twist_y_;
  cmd_vel->linear.x = twist_x_;

  geometry_msgs::msg::Pose2D pose;
  pose.x = current_pose.pose.position.x;
  pose.y = current_pose.pose.position.y;
  pose.theta = tf2::getYaw(current_pose.pose.orientation);

  if (!isCollisionFree(distance, cmd_vel.get(), pose)) {
    stopRobot();
    RCLCPP_WARN(logger_, "Collision Ahead - Exiting DriveOnHeading");
    return nav2_behaviors::Status::FAILED;
  }

  vel_pub_->publish(std::move(cmd_vel));

  return nav2_behaviors::Status::RUNNING;
}

float BackUpFreeSpace::findBestDirection(
  const nav2_msgs::msg::Costmap & costmap, geometry_msgs::msg::Pose2D pose, float start_angle,
  float end_angle, float radius, float angle_increment)
{
  float best_angle = start_angle;

  float first_safe_angle = -1.0f;
  float last_unsafe_angle = -1.0f;

  float final_safe_angle = 0.0f;
  float final_unsafe_angle = 0.0f;

  float resolution = costmap.metadata.resolution;
  float origin_x = costmap.metadata.origin.position.x;
  float origin_y = costmap.metadata.origin.position.y;
  int size_x = costmap.metadata.size_x;
  int size_y = costmap.metadata.size_y;

  float map_min_x = origin_x;
  float map_max_x = origin_x + (size_x * resolution);
  float map_min_y = origin_y;
  float map_max_y = origin_y + (size_y * resolution);

  for (float angle = start_angle; angle <= end_angle; angle += angle_increment) {
    bool is_safe = true;

    for (float r = 0; r <= radius; r += resolution) {
      float x = pose.x + r * std::cos(angle);
      float y = pose.y + r * std::sin(angle);

      if (x >= map_min_x && x <= map_max_x && y >= map_min_y && y <= map_max_y) {
        int i = static_cast<int>((x - origin_x) / resolution);
        int j = static_cast<int>((y - origin_y) / resolution);

        if (i >= 0 && i < size_x && j >= 0 && j < size_y) {
          if (costmap.data[i + j * size_x] >= 253) {
            is_safe = false;
            break;
          }
        } else {
          is_safe = false;
          break;
        }
      } else {
        is_safe = false;
        break;
      }
    }
    if (is_safe && first_safe_angle == -1.0f) {
      first_safe_angle = angle;
    }

    if (!is_safe && first_safe_angle != -1.0f && last_unsafe_angle == -1.0f) {
      last_unsafe_angle = angle;
    }

    if (
      last_unsafe_angle - first_safe_angle > final_unsafe_angle - final_safe_angle &&
      first_safe_angle != -1.0f && last_unsafe_angle != -1.0f) {
      final_safe_angle = first_safe_angle;
      final_unsafe_angle = last_unsafe_angle;
      first_safe_angle = -1.0f;
      last_unsafe_angle = -1.0f;
    }
  }
  best_angle = (final_safe_angle + final_unsafe_angle) / 2.0f;
  RCLCPP_WARN(logger_, "first %f last%f", final_safe_angle, final_unsafe_angle);
  visualizeline(pose, radius, final_safe_angle, final_unsafe_angle);
  return best_angle;
}

std::vector<geometry_msgs::msg::Point> BackUpFreeSpace::gatherFreePoints(
  const nav2_msgs::msg::Costmap & costmap, geometry_msgs::msg::Pose2D pose, float radius)
{
  std::vector<geometry_msgs::msg::Point> results;
  for (unsigned int i = 0; i < costmap.metadata.size_x; i++) {
    for (unsigned int j = 0; j < costmap.metadata.size_y; j++) {
      auto idx = i + j * costmap.metadata.size_x;
      auto x = i * costmap.metadata.resolution + costmap.metadata.origin.position.x;
      auto y = j * costmap.metadata.resolution + costmap.metadata.origin.position.y;
      if (std::hypot(x - pose.x, y - pose.y) <= radius && costmap.data[idx] == 0) {
        geometry_msgs::msg::Point p;
        p.x = x;
        p.y = y;
        results.push_back(p);
      }
    }
  }
  return results;
}

void BackUpFreeSpace::visualize(const geometry_msgs::msg::Point & target_point)
{
  visualization_msgs::msg::MarkerArray markers;

  // Marker for target point
  visualization_msgs::msg::Marker target_marker;
  target_marker.header.frame_id = global_frame_;
  target_marker.header.stamp = clock_->now();
  target_marker.ns = "target_point";
  target_marker.id = 0;
  target_marker.type = visualization_msgs::msg::Marker::SPHERE;
  target_marker.action = visualization_msgs::msg::Marker::ADD;
  target_marker.pose.position = target_point;
  target_marker.pose.orientation.w = 1.0;
  target_marker.scale.x = 0.2;
  target_marker.scale.y = 0.2;
  target_marker.scale.z = 0.2;
  target_marker.color.r = 1.0;
  target_marker.color.g = 0.0;
  target_marker.color.b = 0.0;
  target_marker.color.a = 1.0;
  markers.markers.push_back(target_marker);

  marker_pub_->publish(markers);
}

void BackUpFreeSpace::visualizeline(
  geometry_msgs::msg::Pose2D pose, float radius, float first_safe_angle, float last_unsafe_angle)
{
  visualization_msgs::msg::MarkerArray markers;
  // Marker for first safe ray
  visualization_msgs::msg::Marker safe_ray;
  safe_ray.header.frame_id = global_frame_;
  safe_ray.header.stamp = clock_->now();
  safe_ray.ns = "rays";
  safe_ray.id = 1;
  safe_ray.type = visualization_msgs::msg::Marker::ARROW;
  safe_ray.action = visualization_msgs::msg::Marker::ADD;
  safe_ray.pose.orientation.w = 1.0;
  safe_ray.scale.x = 0.1;
  safe_ray.scale.y = 0.2;
  safe_ray.scale.z = 0.2;
  safe_ray.color.a = 1.0;
  safe_ray.color.r = 0.0;
  safe_ray.color.g = 1.0;
  safe_ray.color.b = 0.0;
  safe_ray.points.resize(2);
  safe_ray.points[0].x = pose.x;
  safe_ray.points[0].y = pose.y;
  safe_ray.points[0].z = 0;
  safe_ray.points[1].x = pose.x + radius * cos(first_safe_angle);
  safe_ray.points[1].y = pose.y + radius * sin(first_safe_angle);
  safe_ray.points[1].z = 0;
  markers.markers.push_back(safe_ray);

  // Marker for first unsafe ray
  visualization_msgs::msg::Marker unsafe_ray = safe_ray;
  unsafe_ray.id = 2;
  unsafe_ray.color.r = 1.0;
  unsafe_ray.color.g = 0.0;
  unsafe_ray.color.b = 0.0;
  unsafe_ray.points[1].x = pose.x + radius * cos(last_unsafe_angle);
  unsafe_ray.points[1].y = pose.y + radius * sin(last_unsafe_angle);
  markers.markers.push_back(unsafe_ray);
  marker_pub_line_->publish(markers);
}

}  // namespace pb_nav2_behaviors

#include "pluginlib/class_list_macros.hpp"
PLUGINLIB_EXPORT_CLASS(pb_nav2_behaviors::BackUpFreeSpace, nav2_core::Behavior)
