
#ifndef CUSTOM_PAITITION_VOXEL_GRID_MAP_H_
#define CUSTOM_PAITITION_VOXEL_GRID_MAP_H_

#include <pcl/common/common.h>
#include <pcl/common/io.h>
#include <pcl/filters/impl/voxel_grid.hpp>
#include <pcl/filters/voxel_grid.h>

namespace pcl {

/** \brief VoxelGridPartition assembles a local 3D grid over a given PointCloud,
 * and downsamples + filters the data.
  *
  * The VoxelGridPartition class creates a *3D voxel grid* (think about a voxel
  * grid as a set of tiny 3D boxes in space) over the input point cloud data.
  * Then, in each *voxel* (i.e., 3D box), all the points present will be
  * approximated (i.e., *downsampled*) with their centroid. This approach is
  * a bit slower than approximating them with the center of the voxel, but it
  * represents the underlying surface more accurately.
  *
  * \author Radu B. Rusu, Bastian Steder
  * \ingroup filters
  */
template <typename PointT> class VoxelGridPartition : public VoxelGrid<PointT> {

protected:
  using Filter<PointT>::initCompute;
  using Filter<PointT>::deinitCompute;
  using VoxelGrid<PointT>::filter_name_;
  using VoxelGrid<PointT>::getClassName;
  using VoxelGrid<PointT>::input_;
  using VoxelGrid<PointT>::indices_;
  using VoxelGrid<PointT>::leaf_size_;
  using VoxelGrid<PointT>::inverse_leaf_size_;
  using VoxelGrid<PointT>::downsample_all_data_;
  using VoxelGrid<PointT>::save_leaf_layout_;
  using VoxelGrid<PointT>::leaf_layout_;
  using VoxelGrid<PointT>::min_b_;
  using VoxelGrid<PointT>::max_b_;
  using VoxelGrid<PointT>::div_b_;
  using VoxelGrid<PointT>::divb_mul_;
  using VoxelGrid<PointT>::filter_field_name_;
  using VoxelGrid<PointT>::filter_limit_min_;
  using VoxelGrid<PointT>::filter_limit_max_;
  using VoxelGrid<PointT>::filter_limit_negative_;
  using VoxelGrid<PointT>::min_points_per_voxel_;

  typedef typename pcl::traits::fieldList<PointT>::type FieldList;
  typedef typename Filter<PointT>::PointCloud PointCloud;
  typedef typename PointCloud::Ptr PointCloudPtr;
  typedef typename PointCloud::ConstPtr PointCloudConstPtr;
  typedef boost::shared_ptr<VoxelGridPartition<PointT>> Ptr;
  typedef boost::shared_ptr<const VoxelGridPartition<PointT>> ConstPtr;

public:
  /** \brief Empty constructor. */
  VoxelGridPartition() { filter_name_ = "VoxelGridPartition"; }

  /** \brief Destructor. */
  virtual ~VoxelGridPartition() {}

public:
  void compute(std::vector<PointCloudPtr> &pc_vector) {
    if (!initCompute())
      return;
    applyPartition(pc_vector);
    deinitCompute();
  }

protected:
  /** \brief Partition a Point Cloud using a voxelized grid approach
    * \param[out] output the resultant point cloud vectors
    */
  void applyPartition(std::vector<PointCloudPtr> &pc_vector);
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename PointT>
void pcl::VoxelGridPartition<PointT>::applyPartition(
    std::vector<PointCloudPtr> &pc_vector) {

  // Has the input dataset been set already?
  if (!input_) {
    PCL_INFO("[pcl::%s::applyFilter] No input dataset given!\n",
             getClassName().c_str());
    pc_vector.clear();
    return;
  }

  Eigen::Vector4f min_p, max_p;
  // Get the minimum and maximum dimensions
  if (!filter_field_name_
           .empty()) // If we don't want to process the entire cloud...
    getMinMax3D<PointT>(input_, *indices_, filter_field_name_,
                        static_cast<float>(filter_limit_min_),
                        static_cast<float>(filter_limit_max_), min_p, max_p,
                        filter_limit_negative_);
  else
    getMinMax3D<PointT>(*input_, *indices_, min_p, max_p);
  // Check that the leaf size is not too small, given the size of the data
  int64_t dx =
      static_cast<int64_t>((max_p[0] - min_p[0]) * inverse_leaf_size_[0]) + 1;
  int64_t dy =
      static_cast<int64_t>((max_p[1] - min_p[1]) * inverse_leaf_size_[1]) + 1;
  int64_t dz =
      static_cast<int64_t>((max_p[2] - min_p[2]) * inverse_leaf_size_[2]) + 1;

  if ((dx * dy * dz) >
      static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
    PCL_INFO("[pcl::applyFilter] Leaf size is too small for the input dataset. "
             "Integer indices would overflow.",
             getClassName().c_str());
    pc_vector.clear();
    return;
  }
  // Compute the minimum and maximum bounding box values
  min_b_[0] = static_cast<int>(floor(min_p[0] * inverse_leaf_size_[0]));
  max_b_[0] = static_cast<int>(floor(max_p[0] * inverse_leaf_size_[0]));
  min_b_[1] = static_cast<int>(floor(min_p[1] * inverse_leaf_size_[1]));
  max_b_[1] = static_cast<int>(floor(max_p[1] * inverse_leaf_size_[1]));
  min_b_[2] = static_cast<int>(floor(min_p[2] * inverse_leaf_size_[2]));
  max_b_[2] = static_cast<int>(floor(max_p[2] * inverse_leaf_size_[2]));

  // Compute the number of divisions needed along all axis
  div_b_ = max_b_ - min_b_ + Eigen::Vector4i::Ones();
  div_b_[3] = 0;

  // Set up the division multiplier
  divb_mul_ = Eigen::Vector4i(1, div_b_[0], div_b_[0] * div_b_[1], 0);

  int centroid_size = 4;
  if (downsample_all_data_)
    centroid_size = boost::mpl::size<FieldList>::value;

  // ---[ RGB special case
  std::vector<pcl::PCLPointField> fields;
  int rgba_index = -1;
  rgba_index = pcl::getFieldIndex(*input_, "rgb", fields);
  if (rgba_index == -1)
    rgba_index = pcl::getFieldIndex(*input_, "rgba", fields);
  if (rgba_index >= 0) {
    rgba_index = fields[rgba_index].offset;
    centroid_size += 3;
  }
  std::vector<cloud_point_index_idx> index_vector;
  index_vector.reserve(indices_->size());

  // If we don't want to process the entire cloud, but rather filter points far
  // away from the viewpoint first...
  if (!filter_field_name_.empty()) {
    // Get the distance field index
    std::vector<pcl::PCLPointField> fields;
    int distance_idx = pcl::getFieldIndex(*input_, filter_field_name_, fields);
    if (distance_idx == -1)
      PCL_INFO(
          "[pcl::%s::applyFilter] Invalid filter field name. Index is %d.\n",
          getClassName().c_str(), distance_idx);

    // First pass: go over all points and insert them into the index_vector
    // vector
    // with calculated idx. Points with the same idx value will contribute to
    // the
    // same point of resulting CloudPoint
    for (std::vector<int>::const_iterator it = indices_->begin();
         it != indices_->end(); ++it) {
      if (!input_->is_dense)
        // Check if the point is invalid
        if (!pcl_isfinite(input_->points[*it].x) ||
            !pcl_isfinite(input_->points[*it].y) ||
            !pcl_isfinite(input_->points[*it].z))
          continue;

      // Get the distance value
      const uint8_t *pt_data =
          reinterpret_cast<const uint8_t *>(&input_->points[*it]);
      float distance_value = 0;
      memcpy(&distance_value, pt_data + fields[distance_idx].offset,
             sizeof(float));

      if (filter_limit_negative_) {
        // Use a threshold for cutting out points which inside the interval
        if ((distance_value < filter_limit_max_) &&
            (distance_value > filter_limit_min_))
          continue;
      } else {
        // Use a threshold for cutting out points which are too close/far away
        if ((distance_value > filter_limit_max_) ||
            (distance_value < filter_limit_min_))
          continue;
      }

      int ijk0 = static_cast<int>(
          floor(input_->points[*it].x * inverse_leaf_size_[0]) -
          static_cast<float>(min_b_[0]));
      int ijk1 = static_cast<int>(
          floor(input_->points[*it].y * inverse_leaf_size_[1]) -
          static_cast<float>(min_b_[1]));
      int ijk2 = static_cast<int>(
          floor(input_->points[*it].z * inverse_leaf_size_[2]) -
          static_cast<float>(min_b_[2]));

      // Compute the centroid leaf index
      int idx = ijk0 * divb_mul_[0] + ijk1 * divb_mul_[1] + ijk2 * divb_mul_[2];
      index_vector.push_back(
          cloud_point_index_idx(static_cast<unsigned int>(idx), *it));
    }
  }
  // No distance filtering, process all data
  else {
    // First pass: go over all points and insert them into the index_vector
    // vector
    // with calculated idx. Points with the same idx value will contribute to
    // the
    // same point of resulting CloudPoint
    for (std::vector<int>::const_iterator it = indices_->begin();
         it != indices_->end(); ++it) {
      if (!input_->is_dense)
        // Check if the point is invalid
        if (!pcl_isfinite(input_->points[*it].x) ||
            !pcl_isfinite(input_->points[*it].y) ||
            !pcl_isfinite(input_->points[*it].z))
          continue;

      int ijk0 = static_cast<int>(
          floor(input_->points[*it].x * inverse_leaf_size_[0]) -
          static_cast<float>(min_b_[0]));
      int ijk1 = static_cast<int>(
          floor(input_->points[*it].y * inverse_leaf_size_[1]) -
          static_cast<float>(min_b_[1]));
      int ijk2 = static_cast<int>(
          floor(input_->points[*it].z * inverse_leaf_size_[2]) -
          static_cast<float>(min_b_[2]));

      // Compute the centroid leaf index
      int idx = ijk0 * divb_mul_[0] + ijk1 * divb_mul_[1] + ijk2 * divb_mul_[2];
      index_vector.push_back(
          cloud_point_index_idx(static_cast<unsigned int>(idx), *it));
    }
  }
  // Second pass: sort the index_vector vector using value representing target
  // cell as index
  // in effect all points belonging to the same output cell will be next to each
  // other
  std::sort(index_vector.begin(), index_vector.end(),
            std::less<cloud_point_index_idx>());

  // Third pass: count output cells
  // we need to skip all the same, adjacenent idx values
  unsigned int total = 0;
  unsigned int index = 0;
  // first_and_last_indices_vector[i] represents the index in index_vector of
  // the first point in
  // index_vector belonging to the voxel which corresponds to the i-th output
  // point,
  // and of the first point not belonging to.
  std::vector<std::pair<unsigned int, unsigned int>>
      first_and_last_indices_vector;
  // Worst case size
  first_and_last_indices_vector.reserve(index_vector.size());
  while (index < index_vector.size()) {
    unsigned int i = index + 1;
    while (i < index_vector.size() &&
           index_vector[i].idx == index_vector[index].idx)
      ++i;
    if (i - index >= min_points_per_voxel_) {
      ++total;
      first_and_last_indices_vector.push_back(
          std::pair<unsigned int, unsigned int>(index, i));
    }
    index = i;
  }

  // Fourth pass: compute centroids, insert them into their final position
  pc_vector.resize(total);
  for (int i = 0; i < total; i++) {
    pc_vector[i].reset(new PointCloud);
  }
  if (save_leaf_layout_) {
    try {
      // Resizing won't reset old elements to -1.  If leaf_layout_ has been used
      // previously, it needs to be re-initialized to -1
      uint32_t new_layout_size = div_b_[0] * div_b_[1] * div_b_[2];
      // This is the number of elements that need to be re-initialized to -1
      uint32_t reinit_size =
          std::min(static_cast<unsigned int>(new_layout_size),
                   static_cast<unsigned int>(leaf_layout_.size()));
      for (uint32_t i = 0; i < reinit_size; i++) {
        leaf_layout_[i] = -1;
      }
      leaf_layout_.resize(new_layout_size, -1);
    } catch (std::bad_alloc &) {
      throw PCLException("VoxelGridPartition bin size is too low; impossible "
                         "to allocate memory for layout",
                         "voxel_grid_partition.hpp", "applyFilter");
    } catch (std::length_error &) {
      throw PCLException("VoxelGridPartition bin size is too low; impossible "
                         "to allocate memory for layout",
                         "voxel_grid_partition.hpp", "applyFilter");
    }
  }

  index = 0;
  for (unsigned int cp = 0; cp < first_and_last_indices_vector.size(); ++cp) {
    // calculate centroid - sum values from all input points, that have the same
    // idx value in index_vector array
    unsigned int first_index = first_and_last_indices_vector[cp].first;
    unsigned int last_index = first_and_last_indices_vector[cp].second;

    for (unsigned int i = first_index; i < last_index; ++i) {
      pc_vector[cp]->push_back(
          input_->points[index_vector[i].cloud_point_index]);
    }

    // index is centroid final position in resulting PointCloud
    if (save_leaf_layout_)
      leaf_layout_[index_vector[first_index].idx] = index;

    ++index;
  }
}
}
#endif //#ifndef CUSTOM_PAITITION_VOXEL_GRID_MAP_H_
