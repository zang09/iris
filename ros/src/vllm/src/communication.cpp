#include "vllm_ros/communication.hpp"
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

namespace vllm_ros
{
// h: [0,360]
// s: [0,1]
// v: [0,1]
Eigen::Vector3f convertRGB(Eigen::Vector3f hsv)
{
  const float max = hsv(2);
  const float min = max * (1 - hsv(1));
  const float H = hsv(0);
  const float D = max - min;
  if (H < 60) return {max, H / 60.f * D + min, min};
  if (H < 120) return {(120.f - H) / 60.f * D + min, max, min};
  if (H < 180) return {min, max, (H - 120) / 60.f * D + min};
  if (H < 240) return {min, (240.f - H) / 60.f * D + min, max};
  if (H < 300) return {(H - 240.f) / 60.f * D + min, min, max};
  if (H < 360) return {max, min, (360.f - H) / 60.f * D + min};
  return {1.0f, 1.0f, 1.0f};
}

void publishPose(const Eigen::Matrix4f& T, const std::string& child_frame_id)
{
  static tf::TransformBroadcaster br;

  tf::Transform transform;
  transform.setFromOpenGLMatrix(T.cast<double>().eval().data());
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "world", child_frame_id));
}

void publishPointcloud(ros::Publisher& publisher, const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud)
{
  auto tmp = cloud;
  pcl_conversions::toPCL(ros::Time::now(), tmp->header.stamp);
  tmp->header.frame_id = "world";
  publisher.publish(tmp);
}

void publishImage(image_transport::Publisher& publisher, const cv::Mat& image)
{
  sensor_msgs::ImagePtr msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", image).toImageMsg();
  publisher.publish(msg);
}

void publishCorrespondences(ros::Publisher& publisher,
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& source,
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& target,
    const pcl::CorrespondencesPtr& correspondences)
{
  visualization_msgs::Marker line_strip;
  line_strip.header.frame_id = "world";
  line_strip.header.stamp = ros::Time::now();
  line_strip.ns = "correspondences";
  line_strip.action = visualization_msgs::Marker::ADD;
  line_strip.pose.orientation.w = 1.0;
  line_strip.id = 0;
  line_strip.scale.x = 0.3;
  line_strip.type = visualization_msgs::Marker::LINE_LIST;
  line_strip.color.r = 1.0;
  line_strip.color.g = 0.0;
  line_strip.color.b = 0.0;
  line_strip.color.a = 1.0;

  for (const pcl::Correspondence& c : *correspondences) {
    pcl::PointXYZ point1 = source->at(c.index_query);
    pcl::PointXYZ point2 = target->at(c.index_match);
    geometry_msgs::Point p1, p2;
    p1.x = point1.x;
    p1.y = point1.y;
    p1.z = point1.z;
    p2.x = point2.x;
    p2.y = point2.y;
    p2.z = point2.z;
    line_strip.points.push_back(p1);
    line_strip.points.push_back(p2);
  }

  publisher.publish(line_strip);
}

visualization_msgs::Marker makeMarkerAsLine(const Eigen::Vector3f& s, const Eigen::Vector3f& e, int id)
{
  visualization_msgs::Marker line_strip;
  line_strip.header.frame_id = "world";
  line_strip.header.stamp = ros::Time::now();
  line_strip.ns = "points_and_lines";
  line_strip.action = visualization_msgs::Marker::ADD;
  line_strip.pose.orientation.w = 1.0;
  line_strip.id = id;
  line_strip.type = visualization_msgs::Marker::LINE_STRIP;
  line_strip.color.a = 1.0;
  line_strip.scale.x = 0.5;

  {
    geometry_msgs::Point p;
    p.x = s.x();
    p.y = s.y();
    p.z = s.z();
    line_strip.points.push_back(p);
  }
  {
    geometry_msgs::Point p;
    p.x = e.x();
    p.y = e.y();
    p.z = e.z();
    line_strip.points.push_back(p);
  }
  return line_strip;
}

void publishTrajectory(ros::Publisher& publisher,
    const std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>>& trajectory, int color)
{
  // visualization_msgs::MarkerArray curve;
  // // delte previous line
  // {
  //   visualization_msgs::Marker reset_line;
  //   reset_line.header.frame_id = "world";
  //   reset_line.header.stamp = ros::Time::now();
  //   reset_line.ns = "points_and_lines";
  //   reset_line.action = visualization_msgs::Marker::DELETEALL;
  //   curve.markers.push_back(reset_line);
  // }

  visualization_msgs::Marker line_strip;
  line_strip.header.frame_id = "world";
  line_strip.header.stamp = ros::Time::now();
  line_strip.ns = "points_and_lines";
  line_strip.action = visualization_msgs::Marker::ADD;
  line_strip.pose.orientation.w = 1.0;
  line_strip.id = 0;
  line_strip.type = visualization_msgs::Marker::LINE_STRIP;
  line_strip.color.a = 1.0;
  line_strip.scale.x = 0.5;

  if (color == 0) {
    line_strip.color.r = 1.0f;
    line_strip.color.g = 0.0f;
    line_strip.color.b = 1.0f;
  }

  if (color == 1) {
    line_strip.color.r = 0.6f;
    line_strip.color.g = 0.6f;
    line_strip.color.b = 0.6f;
  }

  for (const Eigen::Vector3f& t : trajectory) {
    geometry_msgs::Point p;
    p.x = t.x();
    p.y = t.y();
    p.z = t.z();
    line_strip.points.push_back(p);
  }
  publisher.publish(line_strip);
}

std::function<void(const sensor_msgs::ImageConstPtr&)> imageCallbackGenerator(cv::Mat& subscribed_image)
{
  return [&subscribed_image](const sensor_msgs::ImageConstPtr& msg) -> void {
    cv_bridge::CvImagePtr cv_ptr;
    cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    subscribed_image = cv_ptr->image.clone();
  };
}


void publishResetPointcloud(ros::Publisher& publisher)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr tmp(new pcl::PointCloud<pcl::PointXYZ>);
  pcl_conversions::toPCL(ros::Time::now(), tmp->header.stamp);
  tmp->header.frame_id = "world";
  publisher.publish(tmp);
}

void publishResetTrajectory(ros::Publisher& publisher)
{
  visualization_msgs::Marker reset_line;
  reset_line.header.frame_id = "world";
  reset_line.header.stamp = ros::Time::now();
  reset_line.ns = "points_and_lines";
  reset_line.action = visualization_msgs::Marker::DELETEALL;
  publisher.publish(reset_line);
}

void publishResetCorrespondences(ros::Publisher& publisher)
{
  visualization_msgs::Marker line_strip;
  line_strip.header.frame_id = "world";
  line_strip.header.stamp = ros::Time::now();
  line_strip.ns = "correspondences";
  line_strip.action = visualization_msgs::Marker::ADD;
  line_strip.pose.orientation.w = 1.0;
  line_strip.id = 0;
  line_strip.scale.x = 0.3;
  line_strip.type = visualization_msgs::Marker::LINE_LIST;
  line_strip.color.r = 1.0;
  line_strip.color.g = 0.0;
  line_strip.color.b = 0.0;
  line_strip.color.a = 1.0;
  publisher.publish(line_strip);
}


}  // namespace vllm_ros