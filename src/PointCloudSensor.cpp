#include "PointCloudSensor.hpp"
#include "GraphMapper.hpp"

#include "pcl/registration/gicp.h"
#include <pcl/filters/voxel_grid.h>

#define FLT_SIZE 2.0

using namespace slam;

typedef pcl::GeneralizedIterativeClosestPoint<PointType, PointType> ICP;

PointCloudSensor::PointCloudSensor(std::string n, GraphMapper* m, Logger* l)
 : Sensor(n, m, l)
{
	
}

PointCloudSensor::~PointCloudSensor()
{

}

PointCloud::Ptr PointCloudSensor::downsample(PointCloud::ConstPtr in, double leaf_size) const
{
	PointCloud::Ptr out(new PointCloud);
	pcl::VoxelGrid<PointType> grid;
	grid.setLeafSize (leaf_size, leaf_size, leaf_size);
	grid.setInputCloud(in);
	grid.filter(*out);
	return out;
}

TransformWithCovariance PointCloudSensor::calculateTransform(Measurement* source, Measurement* target, Transform guess) const
{	
	// Cast to this sensors measurement type
	PointCloudMeasurement* sourceCloud = dynamic_cast<PointCloudMeasurement*>(source);
	PointCloudMeasurement* targetCloud = dynamic_cast<PointCloudMeasurement*>(target);
	if(!sourceCloud || !targetCloud)
	{
		mLogger->message(ERROR, "Measurement given to calculateTransform() is not a PointCloud!");
		throw BadMeasurementType();
	}
	
	// Downsample the scans
	PointCloud::Ptr filtered_source = downsample(sourceCloud->getPointCloud(), FLT_SIZE);
	PointCloud::Ptr filtered_target = downsample(targetCloud->getPointCloud(), FLT_SIZE);
	
	// Configure Generalized-ICP
	ICP icp;
	icp.setMaxCorrespondenceDistance(mConfiguration.max_correspondence_distance);
	icp.setMaximumIterations(mConfiguration.maximum_iterations);
	icp.setTransformationEpsilon(mConfiguration.transformation_epsilon);
	icp.setEuclideanFitnessEpsilon(mConfiguration.euclidean_fitness_epsilon);
	icp.setCorrespondenceRandomness(mConfiguration.correspondence_randomness);
	icp.setMaximumOptimizerIterations(mConfiguration.maximum_optimizer_iterations);
	icp.setRotationEpsilon(mConfiguration.rotation_epsilon);
	
	// We cannot use the "guess" parameter from align() due to a bug in PCL.
	// Instead we have to shift the source cloud to the target frame before
	// calling align on it.
	// TODO: Change once the issue in PCL is resolved:
	// > https://github.com/PointCloudLibrary/pcl/pull/989
	PointCloud::Ptr shifted_source(new PointCloud);
	pcl::transformPointCloud(*filtered_source, *shifted_source, guess.matrix());
	
	icp.setInputSource(shifted_source);
	icp.setInputTarget(filtered_target);
	PointCloud result;
	icp.align(result);

	// Get estimated transform
	TransformWithCovariance twc;
	twc.transform = Transform::Identity();
	twc.covariance = Covariance::Identity();
	if(icp.hasConverged())// && icp.getFitnessScore() <= mConfiguration.max_fitness_score)
	{
		ICP::Matrix4 tf_matrix = icp.getFinalTransformation();

		// check for nan values (why does this happen?)
		bool valid = (tf_matrix.array() == tf_matrix.array()).all();
		if(!valid)
		{
			mLogger->message(ERROR, "Messurement from ICP contains not numerical values.");
		}else
		{
			Transform tf(tf_matrix.cast<double>());
			double dx = tf.translation()(0);
			double dy = tf.translation()(1);
			double dz = tf.translation()(2);
			double dist = sqrt((dx*dx) + (dy*dy) + (dz*dz));
			dx = guess.translation()(0);
			dy = guess.translation()(1);
			dz = guess.translation()(2);
			double dist2 = sqrt((dx*dx) + (dy*dy) + (dz*dz));
			mLogger->message(DEBUG, (boost::format("ICP shift: %1% | Guess: %2%") % dist % dist2).str());
			twc.transform = guess * tf;
		}
	}else
	{
		mLogger->message(DEBUG, (boost::format("ICP failed! (Fitness-Score: %1% > %2%)") % icp.getFitnessScore() % mConfiguration.max_fitness_score).str());
		throw NoMatch();
	}
	return twc;
}

PointCloud::Ptr PointCloudSensor::getAccumulatedCloud(double resolution)
{
	PointCloud::Ptr accu(new PointCloud);
	VertexList vertices = mMapper->getVerticesFromSensor(mName);
//	int added = 0;
	for(VertexList::reverse_iterator it = vertices.rbegin(); it != vertices.rend(); it++)
	{
		PointCloudMeasurement* pcl = dynamic_cast<PointCloudMeasurement*>((*it)->measurement);
		if(!pcl)
		{
			mLogger->message(ERROR, "Measurement in getAccumulatedCloud() is not a point cloud!");
			throw BadMeasurementType();
		}
		
		PointCloud::Ptr tempCloud(new PointCloud);
		pcl::transformPointCloud(*(pcl->getPointCloud()), *tempCloud, (*it)->corrected_pose.matrix());
		*accu += *tempCloud;
//		added++;
		
//		if(added > 20)
//			break;
	}
	return downsample(accu, resolution);
}