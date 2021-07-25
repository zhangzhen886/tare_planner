/**
 * @file planning_env.cpp
 * @author Chao Cao (ccao1@andrew.cmu.edu)
 * @brief Class that manages the world representation using point clouds
 * @version 0.1
 * @date 2020-06-03
 *
 * @copyright Copyright (c) 2021
 *
 */

#include <planning_env/planning_env.h>
#include <viewpoint_manager/viewpoint_manager.h>

namespace planning_env_ns
{
void PlanningEnvParameters::ReadParameters(ros::NodeHandle& nh)
{
  kStackedCloudDwzLeafSize = misc_utils_ns::getParam<double>(nh, "kStackedCloudDwzLeafSize", 0.2);
  kPlannerCloudDwzLeafSize = misc_utils_ns::getParam<double>(nh, "kPlannerCloudDwzLeafSize", 0.2);
  kCollisionCloudDwzLeafSize = misc_utils_ns::getParam<double>(nh, "kCollisionCloudDwzLeafSize", 0.2);
  kKeyposeGraphCollisionCheckRadius = misc_utils_ns::getParam<double>(nh, "kKeyposeGraphCollisionCheckRadius", 0.4);
  kKeyposeGraphCollisionCheckPointNumThr =
      misc_utils_ns::getParam<int>(nh, "kKeyposeGraphCollisionCheckPointNumThr", 1);

  kKeyposeCloudStackNum = misc_utils_ns::getParam<int>(nh, "kKeyposeCloudStackNum", 5);

  kPointCloudRowNum = misc_utils_ns::getParam<int>(nh, "kPointCloudRowNum", 20);
  kPointCloudColNum = misc_utils_ns::getParam<int>(nh, "kPointCloudColNum", 20);
  kPointCloudLevelNum = misc_utils_ns::getParam<int>(nh, "kPointCloudLevelNum", 10);
  kMaxCellPointNum = misc_utils_ns::getParam<int>(nh, "kMaxCellPointNum", 100000);
  kPointCloudCellSize = misc_utils_ns::getParam<double>(nh, "kPointCloudCellSize", 24.0);
  kPointCloudCellHeight = misc_utils_ns::getParam<double>(nh, "kPointCloudCellHeight", 3.0);
  kPointCloudManagerNeighborCellNum = misc_utils_ns::getParam<int>(nh, "kPointCloudManagerNeighborCellNum", 5);
  kCoverCloudZSqueezeRatio = misc_utils_ns::getParam<double>(nh, "kCoverCloudZSqueezeRatio", 2.0);

  kUseFrontier = misc_utils_ns::getParam<bool>(nh, "kUseFrontier", false);
  kFrontierClusterTolerance = misc_utils_ns::getParam<double>(nh, "kFrontierClusterTolerance", 1.0);
  kFrontierClusterMinSize = misc_utils_ns::getParam<int>(nh, "kFrontierClusterMinSize", 30);
  kExtractFrontierRange.x() = misc_utils_ns::getParam<double>(nh, "kExtractFrontierRangeX", 30);
  kExtractFrontierRange.y() = misc_utils_ns::getParam<double>(nh, "kExtractFrontierRangeY", 30);
  kExtractFrontierRange.z() = misc_utils_ns::getParam<double>(nh, "kExtractFrontierRangeZ", 3);
}

PlanningEnv::PlanningEnv(ros::NodeHandle nh, ros::NodeHandle nh_private, std::string world_frame_id)
  : keypose_cloud_count_(0)
  , vertical_surface_extractor_()
  , vertical_frontier_extractor_()
  , robot_position_update_(false)
{
  parameters_.ReadParameters(nh_private);
  keypose_cloud_stack_.resize(parameters_.kKeyposeCloudStackNum);
  for (int i = 0; i < keypose_cloud_stack_.size(); i++)
  {
    keypose_cloud_stack_[i].reset(new pcl::PointCloud<PlannerCloudPointType>());
  }

  vertical_surface_cloud_stack_.resize(parameters_.kKeyposeCloudStackNum);
  for (int i = 0; i < vertical_surface_cloud_stack_.size(); i++)
  {
    vertical_surface_cloud_stack_[i].reset(new pcl::PointCloud<PlannerCloudPointType>());
  }
  keypose_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/keypose_cloud", world_frame_id);
  stacked_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/stacked_cloud", world_frame_id);
  stacked_vertical_surface_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/stacked_vertical_surface_cloud", world_frame_id);

  stacked_vertical_surface_cloud_kdtree_ =
      pcl::KdTreeFLANN<PlannerCloudPointType>::Ptr(new pcl::KdTreeFLANN<PlannerCloudPointType>());
  vertical_surface_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/coverage_cloud", world_frame_id);
  diff_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/diff_cloud", world_frame_id);
  collision_cloud_ = pcl::PointCloud<pcl::PointXYZI>::Ptr(new pcl::PointCloud<pcl::PointXYZI>);
  terrain_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/terrain_cloud", world_frame_id);
  planner_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/planner_cloud", world_frame_id);

  pointcloud_manager_ = std::make_unique<pointcloud_manager_ns::PointCloudManager>(
      parameters_.kPointCloudRowNum, parameters_.kPointCloudColNum, parameters_.kPointCloudLevelNum,
      parameters_.kMaxCellPointNum, parameters_.kPointCloudCellSize, parameters_.kPointCloudCellHeight,
      parameters_.kPointCloudManagerNeighborCellNum);
  pointcloud_manager_->SetCloudDwzFilterLeafSize() = parameters_.kPlannerCloudDwzLeafSize;

  rolling_occupancy_grid_ = std::make_unique<rolling_occupancy_grid_ns::RollingOccupancyGrid>(nh_private);

  squeezed_planner_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<PlannerCloudPointType>>(
      nh, "planning_env/squeezed_planner_cloud", world_frame_id);
  squeezed_planner_cloud_kdtree_ =
      pcl::KdTreeFLANN<PlannerCloudPointType>::Ptr(new pcl::KdTreeFLANN<PlannerCloudPointType>());

  uncovered_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/uncovered_cloud", world_frame_id);
  uncovered_frontier_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/uncovered_frontier_cloud", world_frame_id);
  frontier_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/frontier_cloud", world_frame_id);
  filtered_frontier_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/filtered_frontier_cloud", world_frame_id);
  occupied_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/occupied_cloud", world_frame_id);
  free_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/free_cloud", world_frame_id);
  unknown_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/unknown_cloud", world_frame_id);

  rolling_occupancy_grid_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/rolling_occupancy_grid_cloud", world_frame_id);
  rolling_frontier_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/rolling_frontier_cloud", world_frame_id);
  rolling_filtered_frontier_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/rolling_filtered_frontier_cloud", world_frame_id);
  rolled_in_occupancy_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/rolled_in_occupancy_cloud", world_frame_id);
  rolled_out_occupancy_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/rolled_out_occupancy_cloud", world_frame_id);

  pointcloud_manager_occupancy_cloud_ = std::make_unique<pointcloud_utils_ns::PCLCloud<pcl::PointXYZI>>(
      nh, "planning_env/pointcloud_manager_occupancy_cloud_", world_frame_id);

  kdtree_frontier_cloud_ = pcl::search::KdTree<pcl::PointXYZI>::Ptr(new pcl::search::KdTree<pcl::PointXYZI>);
  kdtree_rolling_frontier_cloud_ = pcl::search::KdTree<pcl::PointXYZI>::Ptr(new pcl::search::KdTree<pcl::PointXYZI>);

  // Todo: parameterize
  vertical_surface_extractor_.SetRadiusThreshold(0.2);
  vertical_surface_extractor_.SetZDiffMax(2.0);
  vertical_surface_extractor_.SetZDiffMin(parameters_.kStackedCloudDwzLeafSize);
  vertical_frontier_extractor_.SetNeighborThreshold(2);

  Eigen::Vector3d rolling_occupancy_grid_resolution = rolling_occupancy_grid_->GetResolution();
  double vertical_frontier_neighbor_search_radius =
      std::max(rolling_occupancy_grid_resolution.x(), rolling_occupancy_grid_resolution.y());
  vertical_frontier_neighbor_search_radius =
      std::max(vertical_frontier_neighbor_search_radius, rolling_occupancy_grid_resolution.z());
  vertical_frontier_extractor_.SetRadiusThreshold(vertical_frontier_neighbor_search_radius);
  double z_diff_max = vertical_frontier_neighbor_search_radius * 5;
  double z_diff_min = vertical_frontier_neighbor_search_radius;
  vertical_frontier_extractor_.SetZDiffMax(z_diff_max);
  vertical_frontier_extractor_.SetZDiffMin(z_diff_min);
  vertical_frontier_extractor_.SetNeighborThreshold(2);
}

void PlanningEnv::UpdateCollisionCloud()
{
  collision_cloud_->clear();
  for (int i = 0; i < parameters_.kKeyposeCloudStackNum; i++)
  {
    pcl::PointCloud<pcl::PointXYZI>::Ptr cloud_tmp(new pcl::PointCloud<pcl::PointXYZI>());
    pcl::copyPointCloud<PlannerCloudPointType, pcl::PointXYZI>(*vertical_surface_cloud_stack_[i], *cloud_tmp);
    *(collision_cloud_) += *cloud_tmp;
  }
  collision_cloud_downsizer_.Downsize(collision_cloud_, parameters_.kCollisionCloudDwzLeafSize,
                                      parameters_.kCollisionCloudDwzLeafSize, parameters_.kCollisionCloudDwzLeafSize);
}

// "PlanningEnv::UpdateKeyposeCloud"中调用
void PlanningEnv::UpdateFrontiers()
{
  if (parameters_.kUseFrontier)
  {
    prev_robot_position_ = robot_position_;
    rolling_occupancy_grid_->GetFrontier(frontier_cloud_->cloud_, robot_position_, parameters_.kExtractFrontierRange);

    if (!frontier_cloud_->cloud_->points.empty())
    {
      vertical_frontier_extractor_.ExtractVerticalSurface<pcl::PointXYZI, pcl::PointXYZI>(
          frontier_cloud_->cloud_, filtered_frontier_cloud_->cloud_);
    }

    // Cluster frontiers
    if (!filtered_frontier_cloud_->cloud_->points.empty())
    {
      kdtree_frontier_cloud_->setInputCloud(filtered_frontier_cloud_->cloud_);
      std::vector<pcl::PointIndices> cluster_indices;
      pcl::EuclideanClusterExtraction<pcl::PointXYZI> ec;
      ec.setClusterTolerance(parameters_.kFrontierClusterTolerance);
      ec.setMinClusterSize(1);
      ec.setMaxClusterSize(10000);
      ec.setSearchMethod(kdtree_frontier_cloud_);
      ec.setInputCloud(filtered_frontier_cloud_->cloud_);
      ec.extract(cluster_indices);

      pcl::PointIndices::Ptr inliers(new pcl::PointIndices());
      int cluster_count = 0;
      for (int i = 0; i < cluster_indices.size(); i++)
      {
        if (cluster_indices[i].indices.size() < parameters_.kFrontierClusterMinSize)
        {
          continue;
        }
        for (int j = 0; j < cluster_indices[i].indices.size(); j++)
        {
          int point_ind = cluster_indices[i].indices[j];
          filtered_frontier_cloud_->cloud_->points[point_ind].intensity = cluster_count;
          inliers->indices.push_back(point_ind);
        }
        cluster_count++;
      }
      pcl::ExtractIndices<pcl::PointXYZI> extract;
      extract.setInputCloud(filtered_frontier_cloud_->cloud_);
      extract.setIndices(inliers);
      extract.setNegative(false);
      extract.filter(*(filtered_frontier_cloud_->cloud_));
      filtered_frontier_cloud_->Publish();  // "~/filtered_frontier_cloud"
    }
  }
}

void PlanningEnv::UpdateTerrainCloud(const pcl::PointCloud<pcl::PointXYZI>::Ptr& cloud)
{
  if (cloud->points.empty())
  {
    ROS_WARN("Terrain cloud empty");
  }
  else
  {
    terrain_cloud_->cloud_ = cloud;
  }
}

bool PlanningEnv::InCollision(double x, double y, double z) const
{
  if (stacked_cloud_->cloud_->points.empty())
  {
    ROS_WARN("PlanningEnv::InCollision(): collision cloud empty, not checking collision");
    return false;
  }
  PlannerCloudPointType check_point;
  check_point.x = x;
  check_point.y = y;
  check_point.z = z;
  std::vector<int> neighbor_indices;
  std::vector<float> neighbor_sqdist;
  stacked_vertical_surface_cloud_kdtree_->radiusSearch(check_point, parameters_.kKeyposeGraphCollisionCheckRadius,
                                                       neighbor_indices, neighbor_sqdist);
  if (neighbor_indices.size() > parameters_.kKeyposeGraphCollisionCheckPointNumThr)
  {
    return true;
  }
  else
  {
    return false;
  }
}

// "SensorCoveragePlanner3D::UpdateCoveredAreas"中调用
void PlanningEnv::UpdateCoveredArea(const lidar_model_ns::LiDARModel& robot_viewpoint,
                                    const std::shared_ptr<viewpoint_manager_ns::ViewPointManager>& viewpoint_manager)
{
  if (planner_cloud_->cloud_->points.empty())
  {
    std::cout << "Planning cloud empty, cannot update covered area" << std::endl;
    return;
  }
  geometry_msgs::Point robot_position = robot_viewpoint.getPosition();
  double sensor_range = viewpoint_manager->GetSensorRange();  // param: "kSensorRange"(10.0)
  double coverage_occlusion_thr = viewpoint_manager->GetCoverageOcclusionThr();
  double coverage_dilation_radius = viewpoint_manager->GetCoverageDilationRadius();  // "kCoverageDilationRadius"(1.0)
  std::vector<int> covered_point_indices;
  double vertical_fov_ratio = 0.3;  // bigger fov than viewpoints
  double diff_z_max = sensor_range * vertical_fov_ratio;
  double xy_dist_threshold = 3 * (parameters_.kPlannerCloudDwzLeafSize / 2) / 0.3;
  double z_diff_threshold = 3 * parameters_.kPlannerCloudDwzLeafSize;  // "kPlannerCloudDwzLeafSize"(0.2)
  for (int i = 0; i < planner_cloud_->cloud_->points.size(); i++)
  {
    PlannerCloudPointType point = planner_cloud_->cloud_->points[i];
    if (point.g > 0)
    {
      planner_cloud_->cloud_->points[i].g = 255;
      continue;
    }
    // 当前FOV内可见的点云设置为covered
    if (std::abs(point.z - robot_position.z) < diff_z_max)
    {
      if (misc_utils_ns::InFOVSimple(Eigen::Vector3d(point.x, point.y, point.z),
                                     Eigen::Vector3d(robot_position.x, robot_position.y, robot_position.z),
                                     vertical_fov_ratio, sensor_range, xy_dist_threshold, z_diff_threshold))
      {
        if (robot_viewpoint.CheckVisibility<PlannerCloudPointType>(point, coverage_occlusion_thr))
        {
          planner_cloud_->cloud_->points[i].g = 255;
          covered_point_indices.push_back(i);
          continue;
        }
      }
    }
    // mark covered by visited viewpoints(rviz可视化为红色)
    for (const auto& viewpoint_ind : viewpoint_manager->candidate_indices_)
    {
      if (viewpoint_manager->ViewPointVisited(viewpoint_ind))
      {
        if (viewpoint_manager->VisibleByViewPoint<PlannerCloudPointType>(point, viewpoint_ind))
        {
          planner_cloud_->cloud_->points[i].g = 255;
          covered_point_indices.push_back(i);
          break;
        }
      }
    }
  }

  // Dilate the covered area
  squeezed_planner_cloud_->cloud_->clear();
  for (const auto& point : planner_cloud_->cloud_->points)
  {
    PlannerCloudPointType squeezed_point = point;
    squeezed_point.z = point.z / parameters_.kCoverCloudZSqueezeRatio;  // (2.0)
    squeezed_planner_cloud_->cloud_->points.push_back(squeezed_point);
  }
  squeezed_planner_cloud_kdtree_->setInputCloud(squeezed_planner_cloud_->cloud_);

  // 确保附近的点也都mark(填充)为covered
  for (const auto& ind : covered_point_indices)
  {
    PlannerCloudPointType point = planner_cloud_->cloud_->points[ind];
    std::vector<int> nearby_indices;
    std::vector<float> nearby_sqdist;
    squeezed_planner_cloud_kdtree_->radiusSearch(point, coverage_dilation_radius, nearby_indices, nearby_sqdist);
    if (!nearby_indices.empty())
    {
      for (const auto& idx : nearby_indices)
      {
        MY_ASSERT(idx >= 0 && idx < planner_cloud_->cloud_->points.size());
        planner_cloud_->cloud_->points[idx].g = 255;
      }
    }
  }

  // 更新"pointcloud_manager_"中的"pointcloud_grid_"信息
  for (int i = 0; i < planner_cloud_->cloud_->points.size(); i++)
  {
    PlannerCloudPointType point = planner_cloud_->cloud_->points[i];
    if (point.g > 0)
    {
      int cloud_idx = 0;
      int cloud_point_idx = 0;
      pointcloud_manager_->GetCloudPointIndex(i, cloud_idx, cloud_point_idx);
      pointcloud_manager_->UpdateCoveredCloudPoints(cloud_idx, cloud_point_idx);
    }
  }
}

// "SensorCoveragePlanner3D::UpdateCoveredAreas"中调用
void PlanningEnv::GetUncoveredArea(const std::shared_ptr<viewpoint_manager_ns::ViewPointManager>& viewpoint_manager,
                                   int& uncovered_point_num, int& uncovered_frontier_point_num)
{
  // Clear viewpoint covered point list
  for (const auto& viewpoint_ind : viewpoint_manager->candidate_indices_)
  {
    viewpoint_manager->ResetViewPointCoveredPointList(viewpoint_ind);
  }

  // Get uncovered points
  uncovered_cloud_->cloud_->clear();
  uncovered_frontier_cloud_->cloud_->clear();
  uncovered_point_num = 0;
  uncovered_frontier_point_num = 0;
  for (int i = 0; i < planner_cloud_->cloud_->points.size(); i++)
  {
    PlannerCloudPointType point = planner_cloud_->cloud_->points[i];
    if (point.g > 0)
    {
      continue;
    }
    bool observed = false;
    for (const auto& viewpoint_ind : viewpoint_manager->candidate_indices_)
    {
      if (!viewpoint_manager->ViewPointVisited(viewpoint_ind))
      {
        if (viewpoint_manager->VisibleByViewPoint<PlannerCloudPointType>(point, viewpoint_ind))
        {
          // unvisited viewpoint(rviz可视化为非红色)可见的points，加为Uncovered
          viewpoint_manager->AddUncoveredPoint(viewpoint_ind, uncovered_point_num);
          observed = true;
        }
      }
    }
    if (observed)
    {
      pcl::PointXYZI uncovered_point;
      uncovered_point.x = point.x;
      uncovered_point.y = point.y;
      uncovered_point.z = point.z;
      uncovered_point.intensity = i;
      uncovered_cloud_->cloud_->points.push_back(uncovered_point);
      uncovered_point_num++;
    }
  }
  uncovered_cloud_->Publish();

  // Check uncovered frontiers
  if (parameters_.kUseFrontier)
  {
    for (int i = 0; i < filtered_frontier_cloud_->cloud_->points.size(); i++)
    {
      pcl::PointXYZI point = filtered_frontier_cloud_->cloud_->points[i];
      bool observed = false;
      for (const auto& viewpoint_ind : viewpoint_manager->candidate_indices_)
      {
        if (!viewpoint_manager->ViewPointVisited(viewpoint_ind))
        {
          if (viewpoint_manager->VisibleByViewPoint<pcl::PointXYZI>(point, viewpoint_ind))
          {
            viewpoint_manager->AddUncoveredFrontierPoint(viewpoint_ind, uncovered_frontier_point_num);
            observed = true;
          }
        }
      }
      if (observed)
      {
        pcl::PointXYZI uncovered_frontier_point;
        uncovered_frontier_point.x = point.x;
        uncovered_frontier_point.y = point.y;
        uncovered_frontier_point.z = point.z;
        uncovered_frontier_point.intensity = i;
        uncovered_frontier_cloud_->cloud_->points.push_back(uncovered_frontier_point);
        uncovered_frontier_point_num++;
      }
    }
  }
  uncovered_frontier_cloud_->Publish();
}

void PlanningEnv::GetVisualizationPointCloud(pcl::PointCloud<pcl::PointXYZI>::Ptr vis_cloud)
{
  pointcloud_manager_->GetVisualizationPointCloud(vis_cloud);
}

void PlanningEnv::PublishStackedCloud()
{
  stacked_cloud_->Publish();
}

void PlanningEnv::PublishUncoveredCloud()
{
  uncovered_cloud_->Publish();
}

void PlanningEnv::PublishUncoveredFrontierCloud()
{
  uncovered_frontier_cloud_->Publish();
}

}  // namespace planning_env_ns