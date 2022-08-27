#include "depthtection.hpp"

#include <cmath>
#include <geometry_msgs/msg/detail/pose_stamped__struct.hpp>
#include <opencv2/core/matx.hpp>

#include "candidate.hpp"

static void showImage(const std::string &title, const cv::Mat &img) {
  static std::unordered_map<std::string, bool> window_created;
  if (window_created.find(title) == window_created.end()) {
    cv::namedWindow(title, cv::WINDOW_NORMAL);
    cv::resizeWindow(title, img.cols, img.rows);
    window_created[title] = true;
  }
  cv::imshow(title, img);
  cv::waitKey(1);
}

static pcl::PointCloud<pcl::PointXYZ>::Ptr obtainPointCloudFromDepthCrop(const cv::Mat &depth,
                                                                         const cv::Mat &K,
                                                                         const cv::Mat &D);

static cv::Mat cropImageWithDetection(const cv::Mat &img,
                                      const vision_msgs::msg::Detection2D &detection);
static cv::Vec3f get_point_from_depth(const cv::Mat &depth_img, const cv::Point &point,
                                      const cv::Mat &K, const cv::Mat &D);

Depthtection::Depthtection() : Node("depthtection") {
  // Declare node parameters
  this->declare_parameter<std::string>("camera_topic", "camera");
  this->declare_parameter<std::string>("detection_topic", "detection");
  this->declare_parameter<std::string>("computed_pose_topic", "pose_computed");
  this->declare_parameter<std::string>("ground_truth_topic", "");
  this->declare_parameter<std::string>("base_frame", "base_link");
  this->declare_parameter<bool>("show_detection", false);

  // Read parameters
  std::string camera_topic, detection_topic, computed_pose_topic, ground_truth_topic;

  this->get_parameter("camera_topic", camera_topic);
  this->get_parameter("detection_topic", detection_topic);
  this->get_parameter("base_frame", base_frame_);
  this->get_parameter("show_detection", show_detection_);

  this->get_parameter("computed_pose_topic", computed_pose_topic);
  this->get_parameter("ground_truth_topic", ground_truth_topic);

  // Check topic name format
  if (camera_topic.back() == '/') camera_topic.pop_back();

  // Topic subscription
  if (show_detection_) {
    RCLCPP_INFO(this->get_logger(), "Show_detection enabled: Subscribing to %s",
                camera_topic.c_str());
    rgb_img_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
        camera_topic + "/image_raw", 10,
        std::bind(&Depthtection::rgbImageCallback, this, std::placeholders::_1));
  }
  depth_img_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
      camera_topic + "/depth", 10,
      std::bind(&Depthtection::depthImageCallback, this, std::placeholders::_1));
  camera_info_sub_ = this->create_subscription<sensor_msgs::msg::CameraInfo>(
      camera_topic + "/camera_info", rclcpp::SensorDataQoS(),
      std::bind(&Depthtection::cameraInfoCallback, this, std::placeholders::_1));
  detection_sub_ = this->create_subscription<vision_msgs::msg::Detection2DArray>(
      detection_topic, rclcpp::SensorDataQoS(),
      std::bind(&Depthtection::detectionCallback, this, std::placeholders::_1));

  // Topic publication
  pose_pub_ =
      this->create_publisher<geometry_msgs::msg::PoseStamped>(computed_pose_topic, rclcpp::QoS(10));

  // TF listening
  tfCamCatched_ = false;
  tfImuCatched_ = false;
  tfBuffer_ = std::make_unique<tf2_ros::Buffer>(this->get_clock());
  tfListener_ = std::make_shared<tf2_ros::TransformListener>(*tfBuffer_);
}

Depthtection::~Depthtection(void) { cv::destroyAllWindows(); }

void Depthtection::rgbImageCallback(const sensor_msgs::msg::Image::SharedPtr msg) {
  // convert to cv::Mat
  cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  rgb_img_ = cv_ptr->image;
}

void Depthtection::depthImageCallback(const sensor_msgs::msg::Image::SharedPtr msg) {
  // convert to cv::Mat
  cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::TYPE_32FC1);
  depth_img_ = cv_ptr->image;
}

void Depthtection::cameraInfoCallback(const sensor_msgs::msg::CameraInfo::SharedPtr msg) {
  if (!haveCalibration_) {
    K_ = cv::Mat(3, 3, CV_64FC1, (void *)msg->k.data()).clone();
    D_ = cv::Mat(msg->d.size(), 1, CV_64FC1, (void *)msg->d.data()).clone();
    imgSize_.width = msg->width;
    imgSize_.height = msg->height;
    haveCalibration_ = true;
  }
}

void Depthtection::detectionCallback(const vision_msgs::msg::Detection2DArray::SharedPtr msg) {
  // check if image is available
  if (rgb_img_.empty()) {
    RCLCPP_WARN(this->get_logger(), "No RGB image available");
    return;
  }

  for (auto &detection : msg->detections) {
    if (show_detection_) {
      auto center = detection.bbox.center;
      auto width = detection.bbox.size_x;
      auto height = detection.bbox.size_y;
      cv::rectangle(rgb_img_, cv::Point(center.x - width / 2, center.y - height / 2),
                    cv::Point(center.x + width / 2, center.y + height / 2), cv::Scalar(0, 255, 0),
                    2);
      // add text with detection id
      cv::putText(rgb_img_, std::string(detection.id),
                  cv::Point(center.x - width / 2, center.y - height / 2), cv::FONT_HERSHEY_SIMPLEX,
                  0.5, cv::Scalar(0, 255, 0), 1);
    }
    if (detection.results[0].hypothesis.class_id != target_detection_) {
      continue;
    }
    if (!depth_img_.empty() || !haveCalibration_) {
      RCLCPP_WARN(this->get_logger(), "No camera calibration available");
      return;
    }
    RCLCPP_INFO(this->get_logger(), "Detection %s", detection.id.c_str());
    const auto &hypothesis = detection.results[0].hypothesis;
    auto point = extractEstimatedPoint(depth_img_, detection);
    try {
      tf2::Stamped<tf2::Transform> transform;
      geometry_msgs::msg::TransformStamped tf;
      tf = tfBuffer_->lookupTransform("earth", msg->header.frame_id, tf2::TimePointZero);
      tf2::fromMsg(tf, transform);
      tf2::Vector3 v(point.point.x, point.point.y, point.point.z);
      v = transform * v;
      point.header.frame_id = "earth";
      point.header.stamp = msg->header.stamp;
      point.point.x = v.x();
      point.point.y = v.y();
      point.point.z = v.z();
    } catch (tf2::TransformException &ex) {
      RCLCPP_WARN(this->get_logger(), "TF exception: %s", ex.what());
      return;
    }
    auto candidate = match_candidate(candidates_, hypothesis.class_id, point, 1);
    if (!candidate) {
      candidates_.emplace_back(std::make_shared<Candidate>(candidates_.size() + 1, hypothesis.score,
                                                           hypothesis.class_id, point));
      RCLCPP_INFO(this->get_logger(), "New candidate %s", detection.id.c_str());
    } else {
      candidate->confidence = (candidate->confidence + hypothesis.score) / 2;
      candidate->point = point;
      RCLCPP_INFO(this->get_logger(), "Update candidate %s", detection.id.c_str());
      RCLCPP_INFO(this->get_logger(), "Candidate %s", candidate->class_name.c_str());
      RCLCPP_INFO(this->get_logger(), "Candidate point %f %f %f", candidate->point.point.x,
                  candidate->point.point.y, candidate->point.point.z);
    }
  }

  if (show_detection_) {
    showImage("RGB Image", rgb_img_);
  }
}

geometry_msgs::msg::PointStamped Depthtection::extractEstimatedPoint(
    const cv::Mat &depth_img, const vision_msgs::msg::Detection2D &detection) {
  geometry_msgs::msg::PointStamped point_msg;

  auto center = detection.bbox.center;
  auto point = get_point_from_depth(depth_img, cv::Point(center.x, center.y), K_, D_);

  point_msg.point.x = point[0];
  point_msg.point.y = point[1];
  point_msg.point.z = point[2];
  return point_msg;
}

// Auxiliary function to crop image with detection

cv::Mat cropImageWithDetection(const cv::Mat &img, const vision_msgs::msg::Detection2D &detection) {
  auto center = detection.bbox.center;
  auto width = detection.bbox.size_x;
  auto height = detection.bbox.size_y;
  return img(cv::Rect(center.x - width / 2, center.y - height / 2, width, height));
}

// obtain 3D point from depth image
cv::Vec3f get_point_from_depth(const cv::Mat &depth_img, const cv::Point &point, const cv::Mat &K,
                               const cv::Mat &D) {
  double depth = depth_img.at<float>(point.y, point.x);
  if (depth == 0 || depth == std::numeric_limits<float>::infinity()) {
    return cv::Vec3f(0, 0, 0);
  }
  // undistort point using camera calibration

  // TODO: check distortion model
  cv::Vec2f point_undistorted;

  auto cx = K.at<double>(0, 2);
  auto cy = K.at<double>(1, 2);
  auto fx = K.at<double>(0, 0);
  auto fy = K.at<double>(1, 1);

  auto x = (point.x - cx) * depth / fx;
  auto y = (point.y - cy) * depth / fy;
  auto z = depth;

  return cv::Vec3f(x, y, z);
}

bool Depthtection::updateCandidateFromPointCloud(const Candidate::Ptr &candidate,
                                                 const pcl::PointCloud<pcl::PointXYZ>::Ptr &cloud) {
  // WARN HERE POINT CLOUD MUST BE IN EARTH FRAME

  // EASY WAY for testing
  // find the point with the highest z value
  if (!new_detection_) {
    n_images_without_detection_++;
  } else {
    n_images_without_detection_ = 0;
  }

  auto max_z = -std::numeric_limits<float>::max();
  auto max_idx = 0;
  for (auto i = 0; i < cloud->size(); i++) {
    if (cloud->points[i].z > max_z) {
      max_z = cloud->points[i].z;
      max_idx = i;
    }
  }
  candidate->x() = cloud->points[max_idx].x;
  candidate->y() = cloud->points[max_idx].y;
  candidate->z() = cloud->points[max_idx].z;

  return true;
}

void Depthtection::pointCloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr msg) {
  if (current_phase_ == Phase::VISUAL_DETECTION_WITH_DEPTH ||
      current_phase_ != Phase::ONLY_DEPTH_DETECTION) {
    return;
  }

  auto cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromROSMsg(*msg, *cloud);

  // filter cloud when z > 0 in earth frame
  tf2::Stamped<tf2::Transform> earthTf;
  tf2::Stamped<tf2::Transform> base_frame_Tf;
  try {
    geometry_msgs::msg::TransformStamped tf;
    tf = tfBuffer_->lookupTransform("earth", msg->header.frame_id, tf2::TimePointZero);
    tf2::fromMsg(tf, earthTf);
    tf = tfBuffer_->lookupTransform(base_frame_, msg->header.frame_id, tf2::TimePointZero);
    tf2::fromMsg(tf, base_frame_Tf);
  } catch (tf2::TransformException &ex) {
    RCLCPP_ERROR_ONCE(this->get_logger(), "Could not transform %s to %s: %s", "earth",
                      msg->header.frame_id.c_str(), ex.what());
    return;
  }

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered(new pcl::PointCloud<pcl::PointXYZ>);
  cloud_filtered->points.reserve(cloud->points.size());

  for (auto &point : cloud->points) {
    tf2::Vector3 pointLidar(point.x, point.y, point.z);
    const tf2::Vector3 pointEarth = earthTf * pointLidar;
    const tf2::Vector3 pointBase = base_frame_Tf * pointLidar;

    // check NaN values
    if (!std::isfinite(pointEarth.x()) || !std::isfinite(pointEarth.y()) ||
        !std::isfinite(pointEarth.z())) {
      continue;
    }

    const auto candidate_vec = best_candidate_->getEigen();
    const Eigen::Vector3d earthPoint(pointEarth.x(), pointEarth.y(), pointEarth.z());

    // point must be inside an shpere of radius 0.5m around the best candidate
    // else continue with next point
    if ((earthPoint - candidate_vec).norm() > 0.5) {
      continue;
    }
    cloud_filtered->points.emplace_back(pointEarth.x(), pointEarth.y(), pointEarth.z());
  }

  if (cloud_filtered->points.size() < 20) {
    return;
  }

  // obtain candidate from point cloud
  if (!updateCandidateFromPointCloud(best_candidate_, cloud_filtered)) {
    RCLCPP_INFO(this->get_logger(), "Could not update candidate from point cloud");
    return;
  };
  geometry_msgs::msg::TransformStamped tf;
  tf2::Stamped<tf2::Transform> base_frame_respect_earth_tf;
  tf = tfBuffer_->lookupTransform("earth", base_frame_, tf2::TimePointZero);
  tf2::fromMsg(tf, base_frame_respect_earth_tf);

  // if distance to candidate is less than 0.5m change Phase

  const Eigen::Vector3d candidate_vec = best_candidate_->getEigen();
  const Eigen::Vector3d base_frame_respect_earth_vec(base_frame_respect_earth_tf.getOrigin().x(),
                                                     base_frame_respect_earth_tf.getOrigin().y(),
                                                     base_frame_respect_earth_tf.getOrigin().z());
  const auto distance = (candidate_vec - base_frame_respect_earth_vec).norm();
  if (distance < 0.5) {
    current_phase_ = Phase::TOO_NEAR_TO_DETECT;
    RCLCPP_WARN(this->get_logger(), "PHASE: TOO_NEAR_TO_DETECT");
    return;
  }

  // create msg PointCloud2 with the cloud_filtered points
  sensor_msgs::msg::PointCloud2 cloud_filtered_msg;
  pcl::toROSMsg(*cloud_filtered, cloud_filtered_msg);
  cloud_filtered_msg.header = msg->header;
  cloud_filtered_msg.header.frame_id = "earth";

  // publish filtered cloud
  static auto pub = this->create_publisher<sensor_msgs::msg::PointCloud2>("cloud_filtered", 10);
  pub->publish(cloud_filtered_msg);
}

static pcl::PointCloud<pcl::PointXYZ>::Ptr obtainPointCloudFromDepthCrop(const cv::Mat &depth,
                                                                         const cv::Mat &K,
                                                                         const cv::Mat &D) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

  const double fx = K.at<double>(0, 0);
  const double fy = K.at<double>(1, 1);
  const double cx = K.at<double>(0, 2);
  const double cy = K.at<double>(1, 2);

  cloud->points.reserve(depth.rows * depth.cols);
  for (int v = 0; v < depth.rows; v++) {
    for (int u = 0; u < depth.cols; u++) {
      double d = depth.at<float>(v, u);
      if (d == 0 || d == std::numeric_limits<float>::infinity()) {
        continue;
      }
      cv::Vec3f point_undistorted;
      cv::undistortPoints(cv::Mat(cv::Point3f(u, v, d)), point_undistorted, K, D);
      cloud->points.emplace_back(point_undistorted[0] * d / fx, point_undistorted[1] * d / fy, d);
    }
  }
  return cloud;
};

