// Copyright 2021, Steve Macenski
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

#ifndef ROS2_OUSTER__PROCESSORS__POINTCLOUD_PROCESSOR_HPP_
#define ROS2_OUSTER__PROCESSORS__POINTCLOUD_PROCESSOR_HPP_

#include <vector>
#include <memory>
#include <string>
#include <utility>

#include "rclcpp/qos.hpp"

#include "ros2_ouster/conversions.hpp"

#include "sensor_msgs/msg/point_cloud2.hpp"

#include "ros2_ouster/client/client.h"
#include "ros2_ouster/client/lidar_scan.h"
#include "ros2_ouster/client/point.h"
#include "ros2_ouster/interfaces/data_processor_interface.hpp"
#include "ros2_ouster/full_rotation_accumulator.hpp"

#include <tf2_ros/transform_listener.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <pcl_ros/transforms.hpp>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/extract_indices.h>

using Cloud = pcl::PointCloud<ouster_ros::Point>;

namespace sensor
{
/**
 * @class sensor::PointcloudProcessor
 * @brief A data processor interface implementation of a processor
 * for creating Pointclouds in the
 * driver in ROS2.
 */
  class PointcloudProcessor : public ros2_ouster::DataProcessorInterface
  {
  public:
    /**
     * @brief A constructor for sensor::PointcloudProcessor
     * @param node Node for creating interfaces
     * @param mdata metadata about the sensor
     * @param frame frame_id to use for messages
     */
    PointcloudProcessor(
      const rclcpp_lifecycle::LifecycleNode::SharedPtr node,
      const ouster::sensor::sensor_info & mdata,
      const std::string & frame,
      const rclcpp::QoS & qos,
      const ouster::sensor::packet_format & pf,
      std::shared_ptr<sensor::FullRotationAccumulator> fullRotationAccumulator)
    : DataProcessorInterface(), _node(node), _frame(frame)
    {
      _fullRotationAccumulator = fullRotationAccumulator;
      _height = mdata.format.pixels_per_column;
      _width = mdata.format.columns_per_frame;
      _xyz_lut = ouster::make_xyz_lut(mdata);
      _cloud = std::make_unique<Cloud>(_width, _height);
      _pub = _node->create_publisher<sensor_msgs::msg::PointCloud2>(
        "points", qos);
      _pub_base_link = _node->create_publisher<sensor_msgs::msg::PointCloud2>(
        "points_base_link", qos);


      tf_buffer_ = std::make_shared<tf2_ros::Buffer>(node->get_clock());
      tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);
    }

    /**
     * @brief A destructor clearing memory allocated
     */
    ~PointcloudProcessor()
    {
      _pub.reset();
      _pub_base_link.reset();
    }

    template<typename P>
    typename pcl::PointCloud<P>::Ptr cropBox(
      const typename pcl::PointCloud<P>::Ptr cloud,
      Eigen::Vector4f min,
      Eigen::Vector4f max)
    {
      typename pcl::PointCloud<P>::Ptr crop_cloud(new pcl::PointCloud<P>());
      typename pcl::CropBox<P> boxFilter(true);
      boxFilter.setMin(min);
      boxFilter.setMax(max);
      boxFilter.setInputCloud(cloud);
      std::vector<int> indices;
      boxFilter.filter(indices);

      pcl::PointIndices::Ptr inliers_crop{new pcl::PointIndices};
      for (int point : indices) {
        inliers_crop->indices.push_back(point);
      }
      typename pcl::ExtractIndices<P> extract;
      extract.setInputCloud(cloud);
      extract.setIndices(inliers_crop);
      extract.setNegative(true);
      extract.filter(*crop_cloud);
      crop_cloud->height = 1;
      crop_cloud->width = crop_cloud->points.size();
      return crop_cloud;
    }

    /**
     * @brief Process method to create pointcloud
     * @param data the packet data
     */
    bool process(const uint8_t * data, const uint64_t override_ts) override
    {
      if (!_fullRotationAccumulator->isBatchReady()) {
        return true;
      }

      ros2_ouster::toCloud(
        _xyz_lut, _fullRotationAccumulator->getTimestamp(),
        *_fullRotationAccumulator->getLidarScan(), *_cloud);

      auto ros_cloud = ros2_ouster::toMsg(
        *_cloud,
        _fullRotationAccumulator->getTimestamp(),
        _frame, override_ts);

      // pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_curr(new pcl::PointCloud<pcl::PointXYZI>());
      //pcl::fromROSMsg(ros_cloud, *pcl_curr);

      //cropBox<pcl::PointXYZI>(
      //  pcl_curr,
      //  Eigen::Vector4f(-0.5, -0.5, -0.5, 0),
       // Eigen::Vector4f(0.5, 0.5, 0.5, 0));

      //pcl::toROSMsg(*pcl_curr, ros_cloud);

      _pub->publish(ros_cloud);

      try {

        auto transform = tf_buffer_->lookupTransform(
          "base_link",
          _frame,
          tf2::TimePointZero
        );

        pcl_ros::transformPointCloud(
          "base_link", transform, ros_cloud, ros_cloud);

        _pub_base_link->publish(ros_cloud);

      } catch (tf2::TransformException & ex) {
        RCLCPP_ERROR(
          _node->get_logger(),
          "Transform error: %s",
          ex.what());
        return true;
      }


      RCLCPP_DEBUG(
        _node->get_logger(),
        "\n\nCloud published with %s packets\n",
        std::to_string(_fullRotationAccumulator->getPacketsAccumulated()).c_str());

      return true;
    }

    /**
     * @brief Activating processor from lifecycle state transitions
     */
    void onActivate() override
    {
      _pub->on_activate();
      _pub_base_link->on_activate();
    }

    /**
     * @brief Deactivating processor from lifecycle state transitions
     */
    void onDeactivate() override
    {
      _pub->on_deactivate();
      _pub_base_link->on_deactivate();
    }

  private:
    std::unique_ptr<Cloud> _cloud;
    rclcpp_lifecycle::LifecyclePublisher<sensor_msgs::msg::PointCloud2>::SharedPtr _pub;
    rclcpp_lifecycle::LifecyclePublisher<sensor_msgs::msg::PointCloud2>::SharedPtr _pub_base_link;

    rclcpp_lifecycle::LifecycleNode::SharedPtr _node;
    ouster::XYZLut _xyz_lut;
    std::string _frame;
    uint32_t _height;
    uint32_t _width;
    std::shared_ptr<sensor::FullRotationAccumulator> _fullRotationAccumulator;

    std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
    std::shared_ptr<tf2_ros::TransformListener> tf_listener_;
  };

}  // namespace sensor

#endif  // ROS2_OUSTER__PROCESSORS__POINTCLOUD_PROCESSOR_HPP_
