/**
 * @file depthtection.hpp
 * @brief Header file for the depthtection class.
 * @author Miguel Fernandez-Cortizas miguel.fernandez.cortizas@upm.es
 * @date June 2022
 */

#ifndef __DEPTHTECION_HPP__
#define __DEPTHTECION_HPP__

#include <algorithm>
#include <cmath>
#include <geometry_msgs/msg/detail/pose_stamped__struct.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/core/matx.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/qos.hpp>
#include <vector>

#include "as2_msgs/msg/pose_stamped_with_id.hpp"
#include "candidate.hpp"
#include "cv_bridge/cv_bridge.h"
#include "nav_msgs/msg/odometry.hpp"
#include "pcl/common/common.h"
#include "pcl_conversions/pcl_conversions.h"
#include "pcl_ros/transforms.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/camera_info.hpp"
#include "sensor_msgs/msg/fluid_pressure.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/laser_scan.hpp"
#include "tf2/LinearMath/Transform.h"
#include "tf2/exceptions.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "tf2_msgs/msg/tf_message.hpp"
#include "tf2_ros/buffer.h"
#include "tf2_ros/transform_listener.h"
#include "vision_msgs/msg/detection2_d_array.hpp"
#include "std_msgs/msg/string.hpp"

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

class Depthtection : public rclcpp::Node {
  enum Phase {
    NO_DETECTION,
    VISUAL_DETECTION_WITHOUT_DEPTH,
    VISUAL_DETECTION_WITH_DEPTH,
    ONLY_DEPTH_DETECTION,
    TOO_NEAR_TO_DETECT,
  } current_phase_;

  // Camera calibration information
  cv::Size imgSize_;
  cv::Mat K_, D_;
  bool haveCalibration_;

  // Sensor TFs
  std::string base_frame_;
  bool tfCamCatched_, tfImuCatched_;
  tf2::Stamped<tf2::Transform> camBaseTf, imuBaseTf;
  std::shared_ptr<tf2_ros::TransformListener> tfListener_{nullptr};
  std::unique_ptr<tf2_ros::Buffer> tfBuffer_;

  // flags
  bool show_detection_;
  double height_estimation_;
  cv::Mat rgb_img_, depth_img_;
  bool on_running_ = false;

  std::vector<Candidate::Ptr> candidates_;
  Candidate::Ptr best_candidate_;

  int n_images_without_detection_ = 0;
  bool new_detection_ = false;

  std::string target_object_;
  double same_object_distance_threshold_ = 1;
  // Messages

  vision_msgs::msg::Detection2DArray detection_msg_;
  geometry_msgs::msg::PoseStamped best_pose_msg_;
  geometry_msgs::msg::PoseStamped only_depth_pose_msg_;
  geometry_msgs::msg::PoseStamped visual_detection_pose_msg_;
  geometry_msgs::msg::PoseStamped visual_depth_detection_pose_msg_;

  // Data subscribers
  // rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr rgb_img_sub_;
  // rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr depth_img_sub_;
  rclcpp::Subscription<sensor_msgs::msg::CameraInfo>::SharedPtr camera_info_sub_;
  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr point_cloud_sub_;
  // rclcpp::Subscription<vision_msgs::msg::Detection2DArray>::SharedPtr detection_sub_;
  rclcpp::Subscription<geometry_msgs::msg::PoseStamped>::SharedPtr ground_truth_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr phase_sub_;

  // Data publishers
  rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr pose_pub_;

  std::shared_ptr<message_filters::Subscriber<sensor_msgs::msg::Image>> rgb_image_sub_;
  std::shared_ptr<message_filters::Subscriber<sensor_msgs::msg::Image>> depth_img_sub_;
  std::shared_ptr<message_filters::Subscriber<vision_msgs::msg::Detection2DArray>> detection_sub_;

  // typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::msg::Image, sensor_msgs::msg::Image, vision_msgs::msg::Detection2DArray> sync_policy;
  typedef message_filters::sync_policies::ExactTime<sensor_msgs::msg::Image, sensor_msgs::msg::Image, vision_msgs::msg::Detection2DArray> sync_policy;
  std::shared_ptr<message_filters::Synchronizer<sync_policy>> synchronizer_;


  // Methods

  public:
  Depthtection();
  ~Depthtection(void);

  private:
  void pubCandidate(Candidate::Ptr candidate) {
    pose_pub_->publish(*candidate);
    if (has_ground_truth_) {
      Eigen::Vector3d gt_point(ground_truth_pose_msg_.pose.position.x,
                               ground_truth_pose_msg_.pose.position.y,
                               ground_truth_pose_msg_.pose.position.z);
      Eigen::Vector3d candidate_point = candidate->getEigen();
      Eigen::Vector3d distance = (gt_point - candidate_point);
      RCLCPP_INFO(get_logger(), "Distance to ground truth: %f, %f, %f", distance.x(), distance.y(), distance.z());
      RCLCPP_INFO(get_logger(), "Distance to ground truth: %f", distance.norm());
    }
    static auto filtered_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("filtered_pose", 10);
    static auto filtered_pose_msg_ = std::make_shared<geometry_msgs::msg::PoseStamped>();
    filtered_pose_msg_->header = candidate->filtered_point.header;
    filtered_pose_msg_->pose.position = candidate->filtered_point.point;
    filtered_pub_->publish(*filtered_pose_msg_);

    static auto raw_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("raw_pose", 10);
    static auto raw_pose_msg_ = std::make_shared<geometry_msgs::msg::PoseStamped>();
    raw_pose_msg_->header = candidate->raw_point.header;
    raw_pose_msg_->pose.position = candidate->raw_point.point;
    raw_pub_->publish(*raw_pose_msg_);

    static auto compensated_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("compensated_pose", 10);
    static auto compensated_pose_msg_ = std::make_shared<geometry_msgs::msg::PoseStamped>();
    compensated_pose_msg_->header = candidate->compensated_point.header;
    compensated_pose_msg_->pose.position = candidate->compensated_point.point;
    compensated_pub_->publish(*compensated_pose_msg_);
  }

  geometry_msgs::msg::PointStamped extractEstimatedPoint(const cv::Mat& depth_img,
                                                         const vision_msgs::msg::Detection2D& msg);
  bool updateCandidateFromPointCloud(const Candidate::Ptr& candidate,
                                     const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud);
  // Subscribers callbacks
  void rgbImageCallback(const sensor_msgs::msg::Image::SharedPtr msg);
  void depthImageCallback(const sensor_msgs::msg::Image::SharedPtr msg);
  void cameraInfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg);
  void detectionCallback(const vision_msgs::msg::Detection2DArray::SharedPtr msg);
  void pointCloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg);
  bool has_ground_truth_ = false;
  geometry_msgs::msg::PoseStamped ground_truth_pose_msg_;
  void groundTruthCallback(const geometry_msgs::msg::PoseStamped::SharedPtr msg) {
    has_ground_truth_ = true;
    ground_truth_pose_msg_ = *msg;
  };


  void phaseCallback(const std::shared_ptr<std_msgs::msg::String> msg);

  void imagesAndDetectionCallback(const sensor_msgs::msg::Image::SharedPtr img_ptr, const sensor_msgs::msg::Image::SharedPtr depth_ptr, const vision_msgs::msg::Detection2DArray::SharedPtr detection);
};

#endif
