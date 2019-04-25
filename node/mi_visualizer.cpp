#include <ros/ros.h>

#include <nav_msgs/OccupancyGrid.h>
#include <geometry_msgs/PointStamped.h>
#include <sensor_msgs/PointCloud.h>
#include <visualization_msgs/Marker.h>

#include "wandering_robot/grid_wanderer.hpp"

class MutualInformationVisualizer {

  public:
MutualInformationVisualizer() {
      // Initialize the node handle
      n = ros::NodeHandle("~");

      // Fetch the ROS parameters
      std::string mi_topic, p_not_measured_topic, mi_points_topic, states_topic;
      std::string map_topic, click_topic, trajectory_topic;
      n.getParam("mi_topic", mi_topic);
      n.getParam("p_not_measured_topic", p_not_measured_topic);
      n.getParam("mi_points_topic", mi_points_topic);
      n.getParam("map_topic", map_topic);
      n.getParam("map_incomplete_topic", states_topic);
      n.getParam("trajectory_topic", trajectory_topic);
      n.getParam("click_topic", click_topic);
      n.getParam("mi_angular_steps", mi_angular_steps);
      n.getParam("mi_spatial_steps", mi_spatial_steps);
      n.getParam("condition_steps", condition_steps);
      n.getParam("unknown_threshold", unknown_threshold);
      n.getParam("poisson_rate", poisson_rate);
      n.getParam("beam_independence", beam_independence);

      // Construct a publisher for mutual information
      mi_pub = n.advertise<nav_msgs::OccupancyGrid>(mi_topic, 1, true);
      p_not_measured_pub = n.advertise<nav_msgs::OccupancyGrid>(p_not_measured_topic, 1, true);
      states_pub = n.advertise<nav_msgs::OccupancyGrid>(states_topic, 1, true);
      mi_points_pub = n.advertise<sensor_msgs::PointCloud>(mi_points_topic, 1, true);
      trajectory_pub = n.advertise<visualization_msgs::Marker>(trajectory_topic, 1, true);

      // Subscribe to maps and clicked points
      map_sub = n.subscribe(map_topic, 1, &MutualInformationVisualizer::map_callback, this);
      click_sub = n.subscribe(click_topic, 1, &MutualInformationVisualizer::click_callback, this);
    }

    void map_callback(const nav_msgs::OccupancyGrid & map_msg) {
      // Store map information
      map_info = map_msg.info;
      map_frame_id = map_msg.header.frame_id;

      // Convert to states
      std::vector<wandering_robot::OccupancyState> states(map_info.height * map_info.width);
      for (unsigned int i = 0; i < states.size(); i++) {
        double value = map_msg.data[i]/100.;
        if (value < 0) {
          states[i] = wandering_robot::OccupancyState::unknown;
        } else if (value < 0.5 - unknown_threshold) {
          states[i] = wandering_robot::OccupancyState::free;
        } else if (value > 0.5 + unknown_threshold) {
          states[i] = wandering_robot::OccupancyState::occupied;
        } else {
          states[i] = wandering_robot::OccupancyState::unknown;
        }
      }
      
      // Initialize the grid wanderer
      w = wandering_robot::GridWanderer(
          map_info.height,
          map_info.width,
          poisson_rate,
          beam_independence);
      w.set_map(states);

      draw_map();
    }

    void click_callback(const geometry_msgs::PointStamped & click_msg) {
      unsigned int num_beams = 1000;
      unsigned int num_points = 5;
      double x = click_msg.point.x;
      double y = click_msg.point.y;

      while (ros::ok()) {
        geometry_msgs::Point point;
        point.x = x;
        point.y = y;
        trajectory_msg.points.push_back(point);

        // Make and apply a scan
        std::vector<double> scan = w.make_scan(x, y, num_beams);
        w.apply_scan(x, y, scan);

        // Clear
        w.reset_p_not_measured();
        mi_points_msg.points.clear();

        // Find the closest point with lots of information
        double closest_dist = map_info.height * map_info.width;
        double closest_x, closest_y;
        for (unsigned int i = 0; i < num_points and ros::ok(); i++) {
          // Compute the mutual information
          compute_mi();

          // Find free space with maximum mutual information
          int mi_max_cell = 0;
          double mi_max = 0;
          for (unsigned int j = 0; j < w.mi().size(); j++) {
            if (w.states()[j] == wandering_robot::OccupancyState::free) {
              if (w.mi()[j] > mi_max) {
                mi_max = w.mi()[j];
                mi_max_cell = j;
              }
            }
          }
          std::cout << "max mi " << mi_max << "@" << mi_max_cell << std::endl;
          int y_ = mi_max_cell/map_info.width;
          int x_ = mi_max_cell - y_ * map_info.width;

          // If the point is closer than the current location update
          double dist = std::sqrt((x - x_) * (x - x_) + (y - y_) * (y - y_));
          if (dist < closest_dist) {
            closest_dist = dist;
            closest_x = x_;
            closest_y = y_;
          }

          // Add the point to list of points for visualization
          geometry_msgs::Point32 point;
          point.x = x_;
          point.y = y_;
          mi_points_msg.points.push_back(point);

          // Condition on the maximum point
          w.condition(x_, y_, condition_steps);
        }

        // Update the best point
        x = closest_x;
        y = closest_y;

        draw_map();
      }
    }

    void compute_mi() {
      w.reset_mi();

      for (int i = 0; i < mi_angular_steps; i++) {
        for (int j = 0; j < mi_spatial_steps; j++) {
          w.accrue_mi(j/((double)mi_spatial_steps), i/((double)mi_angular_steps));
        }
        draw_map();
        if (not ros::ok()) break;
      }
    }

    void draw_map() {
      // Construct a message for the mutual information surface
      nav_msgs::OccupancyGrid mi_msg;
      mi_msg.data = std::vector<int8_t>(map_info.height * map_info.width);
      auto mi_max = std::max_element(w.mi().begin(), w.mi().end());
      for (size_t i = 0; i < mi_msg.data.size(); i++) {
        mi_msg.data[i] = 100 * (1 - w.mi()[i]/(*mi_max));
      }

      // Add info
      mi_msg.header.frame_id = map_frame_id;
      mi_msg.header.stamp = ros::Time::now();
      mi_msg.info = map_info;

      // Publish
      mi_pub.publish(mi_msg);

      // Do the same for p_not measured
      nav_msgs::OccupancyGrid p_not_measured_msg = mi_msg;
      for (size_t i = 0; i < p_not_measured_msg.data.size(); i++)
        p_not_measured_msg.data[i] = 100 * (1 - w.p_not_measured()[i]);
      p_not_measured_pub.publish(p_not_measured_msg);

      // Do the same for the states
      nav_msgs::OccupancyGrid states_msg = mi_msg;
      for (size_t i = 0; i < states_msg.data.size(); i++) {
        if (w.states()[i] == wandering_robot::OccupancyState::unknown) {
          states_msg.data[i] = 50;
        } else if (w.states()[i] == wandering_robot::OccupancyState::free) {
          states_msg.data[i] = 0;
        } else {
          states_msg.data[i] = 100;
        }
      }
      states_pub.publish(states_msg);

      // Plot the position of the maximum mutual information
      mi_points_msg.header = mi_msg.header;
      mi_points_pub.publish(mi_points_msg);

      // Plot the trajectory
      trajectory_msg.header = mi_msg.header;
      trajectory_msg.type = visualization_msgs::Marker::LINE_STRIP;
      trajectory_msg.action = visualization_msgs::Marker::ADD;
      trajectory_msg.pose.orientation.w = 1;
      trajectory_msg.scale.x = 3;
      trajectory_msg.color.a = 1;
      trajectory_msg.color.b = 1;
      trajectory_pub.publish(trajectory_msg);
    }

  private:
    ros::NodeHandle n;
    ros::Subscriber map_sub, click_sub;
    ros::Publisher mi_pub, p_not_measured_pub, states_pub;
    ros::Publisher mi_points_pub, trajectory_pub;

    // Parameters
    double poisson_rate, unknown_threshold;
    bool beam_independence;
    int mi_spatial_steps, mi_angular_steps;
    int condition_steps;

    // Map data
    nav_msgs::MapMetaData map_info;
    std::string map_frame_id;

    // Accumulated points
    sensor_msgs::PointCloud mi_points_msg;
    visualization_msgs::Marker trajectory_msg;

    // Computation devices
    wandering_robot::GridWanderer w;
};

int main(int argc, char ** argv) {
  ros::init(argc, argv, "mutual_information_visualizer");
  MutualInformationVisualizer mi;
  ros::spin();
  return 0;
}
