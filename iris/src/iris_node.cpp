// Copyright (c) 2020, Map IV, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of the Map IV, Inc. nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "core/types.hpp"
#include "map/map.hpp"
#include "publish/publish.hpp"
#include "system/system.hpp"
#include <chrono>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <image_transport/image_transport.h>
#include <pcl_ros/point_cloud.h>
#include <ros/ros.h>
#include <std_msgs/Float32.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

// TODO:
pcl::PointCloud<pcl::PointXYZINormal>::Ptr vslam_data(new pcl::PointCloud<pcl::PointXYZINormal>);
bool vslam_update = false;
void callback(const pcl::PointCloud<pcl::PointXYZINormal>::ConstPtr& msg)
{
  *vslam_data = *msg;
  if (vslam_data->size() > 0)
    vslam_update = true;
}

// TODO:
Eigen::Matrix4f listenTransform(tf::TransformListener& listener)
{
  tf::StampedTransform transform;
  try {
    listener.lookupTransform("world", "iris/vslam_pose", ros::Time(0), transform);
  } catch (...) {
  }

  double data[16];
  transform.getOpenGLMatrix(data);
  Eigen::Matrix4d T(data);
  return T.cast<float>();
}

// TODO:
Eigen::Matrix4f T_recover = Eigen::Matrix4f::Zero();
pcl::PointCloud<pcl::PointXYZ>::Ptr current_pointcloud = nullptr;
void callbackForRecover(const geometry_msgs::PoseWithCovarianceStampedConstPtr& msg)
{
  ROS_INFO("It subscribes initial_pose");

  float x = static_cast<float>(msg->pose.pose.position.x);
  float y = static_cast<float>(msg->pose.pose.position.y);
  float qw = static_cast<float>(msg->pose.pose.orientation.w);
  float qz = static_cast<float>(msg->pose.pose.orientation.z);

  float z = std::numeric_limits<float>::max();

  if (current_pointcloud != nullptr) {
    z = 0;
  } else {
    for (const pcl::PointXYZ& p : *current_pointcloud) {
      constexpr float r2 = 2 * 2;  // [m^2]
      float dx = x - p.x;
      float dy = y - p.y;
      if (dx * dx + dy * dy < r2)
        z = std::min(z, p.z);
    }
  }

  T_recover.setIdentity();
  T_recover(0, 3) = x;
  T_recover(1, 3) = y;
  T_recover(2, 3) = z;
  float theta = 2 * std::atan2(qz, qw);
  Eigen::Matrix3f R;
  R << 0, 0, 1,
      -1, 0, 0,
      0, -1, 0;
  T_recover.topLeftCorner(3, 3) = Eigen::AngleAxisf(theta, Eigen::Vector3f::UnitZ()).toRotationMatrix() * R;
}

int main(int argc, char* argv[])
{
  ros::init(argc, argv, "iris_node");
  ros::NodeHandle nh;

  // Setup for vslam_data
  iris::pcXYZ::Ptr vslam_points(new iris::pcXYZ);
  iris::pcNormal::Ptr vslam_normals(new iris::pcNormal);
  std::vector<float> vslam_weights;

  // Setup subscriber
  ros::Subscriber vslam_subscriber = nh.subscribe<pcl::PointCloud<pcl::PointXYZINormal>>("iris/vslam_data", 5, callback);
  ros::Subscriber recover_pose_subscriber = nh.subscribe<geometry_msgs::PoseWithCovarianceStamped>("/initialpose", 5, callbackForRecover);
  tf::TransformListener listener;

  // Setup publisher
  ros::Publisher target_pc_publisher = nh.advertise<pcl::PointCloud<pcl::PointXYZI>>("iris/target_pointcloud", 1, true);
  ros::Publisher whole_pc_publisher = nh.advertise<pcl::PointCloud<pcl::PointXYZI>>("iris/whole_pointcloud", 1, true);
  ros::Publisher source_pc_publisher = nh.advertise<pcl::PointCloud<pcl::PointXYZI>>("iris/source_pointcloud", 1);
  ros::Publisher iris_trajectory_publisher = nh.advertise<visualization_msgs::Marker>("iris/iris_trajectory", 1);
  ros::Publisher vslam_trajectory_publisher = nh.advertise<visualization_msgs::Marker>("iris/vslam_trajectory", 1);
  ros::Publisher correspondences_publisher = nh.advertise<visualization_msgs::Marker>("iris/correspondences", 1);
  ros::Publisher scale_publisher = nh.advertise<std_msgs::Float32>("iris/align_scale", 1);
  ros::Publisher normal_publisher = nh.advertise<visualization_msgs::MarkerArray>("iris/normals", 1);
  ros::Publisher covariance_publisher = nh.advertise<visualization_msgs::MarkerArray>("iris/covariances", 1);
  ros::Publisher processing_time_publisher = nh.advertise<std_msgs::Float32>("iris/processing_time", 1);
  iris::Publication publication;

  // Get rosparams
  ros::NodeHandle pnh("~");
  std::string config_path, pcd_path;
  pnh.getParam("iris_config_path", config_path);
  pnh.getParam("pcd_path", pcd_path);
  ROS_INFO("config_path: %s, pcd_path: %s", config_path.c_str(), pcd_path.c_str());

  // Initialize config
  iris::Config config(config_path);

  // Load LiDAR map
  iris::map::Parameter map_param(
      pcd_path, config.voxel_grid_leaf, config.normal_search_leaf, config.submap_grid_leaf);
  std::shared_ptr<iris::map::Map> map = std::make_shared<iris::map::Map>(map_param, config.T_init);

  // Initialize system
  std::shared_ptr<iris::System> system = std::make_shared<iris::System>(config, map);

  std::chrono::system_clock::time_point m_start;
  ros::Rate loop_rate(10);
  Eigen::Matrix4f offseted_vslam_pose = config.T_init;
  Eigen::Matrix4f iris_pose = config.T_init;

  // Publish map
  iris::publishPointcloud(whole_pc_publisher, map->getSparseCloud());


  // Start main loop
  ROS_INFO("start main loop.");
  while (ros::ok()) {

    Eigen::Matrix4f T_vslam = listenTransform(listener);
    if (!T_recover.isZero()) {
      system->specifyTWorld(T_recover);
      T_recover.setZero();
    }

    if (vslam_update) {
      vslam_update = false;
      m_start = std::chrono::system_clock::now();

      // Execution
      system->execute(2, T_vslam, vslam_data);

      // Publish for rviz
      system->popPublication(publication);
      iris::publishPointcloud(source_pc_publisher, publication.cloud);
      iris::publishTrajectory(iris_trajectory_publisher, publication.iris_trajectory, {1.0f, 0.0f, 1.0f});
      iris::publishTrajectory(vslam_trajectory_publisher, publication.offset_trajectory, {0.6f, 0.6f, 0.6f});
      iris::publishCorrespondences(correspondences_publisher, publication.cloud, map->getTargetCloud(), publication.correspondences);
      iris::publishNormal(normal_publisher, publication.cloud, publication.normals);
      iris::publishCovariance(covariance_publisher, publication.cloud, publication.normals);

      // Processing time
      long time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - m_start).count();
      std::stringstream ss;
      ss << "processing time= \033[35m"
         << time
         << "\033[m ms";
      ROS_INFO("Iris/ALIGN: %s", ss.str().c_str());

      {
        std_msgs::Float32 scale;
        scale.data = iris::util::getScale(publication.T_align);
        scale_publisher.publish(scale);

        std_msgs::Float32 processing_time;
        processing_time.data = static_cast<float>(time);
        processing_time_publisher.publish(processing_time);
      }

      offseted_vslam_pose = publication.offset_camera;
      iris_pose = publication.iris_camera;
      current_pointcloud = publication.cloud;
    }

    iris::publishPose(offseted_vslam_pose, "iris/offseted_vslam_pose");
    iris::publishPose(iris_pose, "iris/iris_pose");
    iris::publishPointcloud(target_pc_publisher, map->getTargetCloud());

    // Spin and wait
    ros::spinOnce();
    loop_rate.sleep();
  }

  ROS_INFO("Finalize the system");
  return 0;
}
