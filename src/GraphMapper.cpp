#include "GraphMapper.hpp"
#include "Solver.hpp"

#include <boost/format.hpp>
#include <graph_analysis/lemon/DirectedGraph.hpp>
#include <graph_analysis/GraphIO.hpp>

#define PI 3.141592654

using namespace slam;

// Re-orthogonalize the rotation-matrix
// http://stackoverflow.com/questions/23080791/eigen-re-orthogonalization-of-rotation-matrix
Transform orthogonalize(const Transform& t)
{
	Vector3 x(t(0,0), t(0,1), t(0,2));
	Vector3 y(t(1,0), t(1,1), t(1,2));
	Vector3 z(t(2,0), t(2,1), t(2,2));
	ScalarType error = x.dot(y);
	
	Vector3 x_ort = x - (error/2.0) * y;
	Vector3 y_ort = y - (error/2.0) * x;
	Vector3 z_ort = x_ort.cross(y_ort);

	Transform res = t;
	ScalarType xdot = 0.5 * (3.0 - x_ort.dot(x_ort));
	res(0,0) = xdot * x_ort(0);
	res(0,1) = xdot * x_ort(1);
	res(0,2) = xdot * x_ort(2);
	
	ScalarType ydot = 0.5 * (3.0 - y_ort.dot(y_ort));
	res(1,0) = ydot * y_ort(0);
	res(1,1) = ydot * y_ort(1);
	res(1,2) = ydot * y_ort(2);
	
	ScalarType zdot = 0.5 * (3.0 - z_ort.dot(z_ort));
	res(2,0) = zdot * z_ort(0);
	res(2,1) = zdot * z_ort(1);
	res(2,2) = zdot * z_ort(2);
	
	return res;
}

VertexObject::Ptr GraphMapper::fromBaseGraph(graph_analysis::Vertex::Ptr base)
{
	VertexObject::Ptr v = boost::dynamic_pointer_cast<VertexObject>(base);
	if(!v)
	{
		throw BadElementType();
	}
	return v;
}

EdgeObject::Ptr GraphMapper::fromBaseGraph(graph_analysis::Edge::Ptr base)
{
	EdgeObject::Ptr e = boost::dynamic_pointer_cast<EdgeObject>(base);
	if(!e)
	{
		throw BadElementType();
	}
	return e;
}

GraphMapper::GraphMapper(Logger* log)
 : mPoseGraph( new graph_analysis::lemon::DirectedGraph()),
   mIndex(flann::KDTreeSingleIndexParams())
{
	mOdometry = NULL;
	mSolver = NULL;
	mLogger = log;
	
	mNeighborRadius = 1.0;
	mMinTranslation = 0.5;
	mMinRotation = 0.1;
	mAddOdometryEdges = false;

	mCurrentPose = Transform::Identity();
}

GraphMapper::~GraphMapper()
{
}

void GraphMapper::setSolver(Solver* solver)
{
	mSolver = solver;
}

void GraphMapper::setOdometry(Odometry* odom, bool add_edges)
{
	mOdometry = odom;
	mAddOdometryEdges = add_edges;
}

bool GraphMapper::optimize()
{
	if(!mSolver)
	{
		mLogger->message(ERROR, "A solver must be set before optimize() is called!");
		return false;
	}

	// Optimize
	if(!mSolver->compute())
	{
		return false;
	}

	// Retrieve results
	IdPoseVector res = mSolver->getCorrections();
	for(IdPoseVector::iterator it = res.begin(); it < res.end(); it++)
	{
		unsigned int id = it->first;
		Transform tf = it->second;
		VertexObject::Ptr v = fromBaseGraph(mPoseGraph->getVertex(id));
		v->corrected_pose = tf;
	}
	
	if(mLastVertex)
	{
		mCurrentPose = mLastVertex->corrected_pose;
	}
	return true;
}

void GraphMapper::registerSensor(Sensor* s)
{
	std::pair<SensorList::iterator, bool> result;
	result = mSensors.insert(SensorList::value_type(s->getName(), s));
	if(!result.second)
	{
		mLogger->message(ERROR, (boost::format("Sensor with name %1% already exists!") % s->getName()).str());
		return;
	}
}

VertexObject::Ptr GraphMapper::addVertex(Measurement* m, const Transform &corrected)
{
	// Create the new VertexObject and add it to the PoseGraph
	boost::format v_name("%1%:%2%");
	v_name % m->getRobotName() % m->getSensorName();
	VertexObject::Ptr newVertex(new VertexObject(v_name.str()));
	newVertex->corrected_pose = corrected;
	newVertex->measurement = m;
	mPoseGraph->addVertex(newVertex);
	
	// Add it to the Index, so we can find it by its unique id
	mVertexIndex.insert(VertexIndex::value_type(m->getUniqueId(), newVertex));
	
	// Add it to the SLAM-Backend for incremental optimization
	if(mSolver)
	{
		graph_analysis::GraphElementId id = mPoseGraph->getVertexId(newVertex);
		mLogger->message(INFO, (boost::format("Created vertex %1% (from %2%:%3%).") % id % m->getRobotName() % m->getSensorName()).str());
		mSolver->addNode(id, newVertex->corrected_pose);
	}
	
	// Set it as fixed in the solver
	if(!mFirstVertex)
	{
		mFirstVertex = newVertex;
		if(mSolver)
		{
			graph_analysis::GraphElementId id = mPoseGraph->getVertexId(newVertex);
			mSolver->setFixed(id);
		}
	}
	
	return newVertex;
}

EdgeObject::Ptr GraphMapper::addEdge(VertexObject::Ptr source, VertexObject::Ptr target,
	const Transform &t, const Covariance &c, const std::string& sensor, const std::string& label)
{
	EdgeObject::Ptr edge(new EdgeObject(sensor, label));
	edge->setSourceVertex(source);
	edge->setTargetVertex(target);
	edge->transform = t;
	edge->covariance = c;
	mPoseGraph->addEdge(edge);
	
	if(mSolver)
	{
		unsigned source_id = mPoseGraph->getVertexId(source);
		unsigned target_id = mPoseGraph->getVertexId(target);
		mLogger->message(INFO, (boost::format("Created '%4%' edge from node %1% to node %2% (from %3%).") % source_id % target_id % sensor % label).str());
		mSolver->addConstraint(source_id, target_id, t, c);
	}
	return edge;
}

bool GraphMapper::addReading(Measurement* m)
{
	// Get the sensor responsible for this measurement
	// Can throw std::out_of_range if sensor is not registered
	Sensor* sensor = NULL;
	try
	{
		sensor = mSensors.at(m->getSensorName());
		mLogger->message(DEBUG, (boost::format("Add reading from own Sensor '%1%'.") % m->getSensorName()).str());
	}catch(std::out_of_range e)
	{
		mLogger->message(ERROR, (boost::format("Sensor '%1%' has not been registered!") % m->getSensorName()).str());
		return false;
	}

	// Get the odometric pose for this measurement
	Transform odometry = Transform::Identity();
	if(mOdometry)
	{
		try
		{
			odometry = mOdometry->getOdometricPose(m->getTimestamp());
		}catch(OdometryException &e)
		{
			mLogger->message(ERROR, "Could not get Odometry data!");
			return false;
		}
	}

	// If this is the first vertex, add it and return
	if(!mLastVertex)
	{
		mLastVertex = addVertex(m, mCurrentPose);
		mLastOdometricPose = odometry;
		mLogger->message(INFO, "Added first node to the graph.");
		return true;
	}

	// Now we have a node, that is not the first and has not been added yet
	VertexObject::Ptr newVertex;
	
	if(mOdometry)
	{
		Transform odom_dist = orthogonalize(mLastOdometricPose.inverse() * odometry);
		mCurrentPose = mLastVertex->corrected_pose * odom_dist;
		if(!checkMinDistance(odom_dist))
			return false;

		if(mAddOdometryEdges)
		{
			// Add the vertex to the pose graph
			newVertex = addVertex(m, orthogonalize(mCurrentPose));

			// Add an edge representing the odometry information
			addEdge(mLastVertex, newVertex, odom_dist, Covariance::Identity(), "Odometry", "odom");
		}
	}

	// Add edge to previous measurement
	if(mLastVertex)
	{
		try
		{
			Transform guess = mLastVertex->corrected_pose.inverse() * mCurrentPose;
			TransformWithCovariance twc = sensor->calculateTransform(mLastVertex->measurement, m, guess);
			mCurrentPose = orthogonalize(mLastVertex->corrected_pose * twc.transform);
			
			if(!newVertex)
			{
				if(!checkMinDistance(twc.transform))
					return false;
				newVertex = addVertex(m, mCurrentPose);
			}
			addEdge(mLastVertex, newVertex, twc.transform, twc.covariance, sensor->getName(), "seq");
		}catch(NoMatch &e)
		{
			if(!newVertex)
			{
				mLogger->message(WARNING, "Measurement could not be matched and no odometry was availabe!");
				return false;
			}
		}
	}

	// Add edges to other measurements nearby
	buildNeighborIndex(sensor->getName());
	linkToNeighbors(newVertex, sensor, 5);

	// Overall last vertex
	mLastVertex = newVertex;
	mLastOdometricPose = odometry;
	return true;
}

void GraphMapper::addExternalReading(Measurement* m, const Transform& t)
{
	VertexObject::Ptr v = addVertex(m, t);
		
	// Get the sensor responsible for this measurement
	// Can throw std::out_of_range if sensor is not registered
	mLogger->message(DEBUG, (boost::format("Add external reading from %1%:%2%.") % m->getRobotName() % m->getSensorName()).str());
	Sensor* sensor = NULL;
	try
	{
		sensor = mSensors.at(m->getSensorName());
		buildNeighborIndex(sensor->getName());
		linkToNeighbors(v, sensor, 5);
	}catch(std::out_of_range e)
	{
		return;
	}
}

void GraphMapper::linkToNeighbors(VertexObject::Ptr vertex, Sensor* sensor, int max_links)
{
	// Get all edges to/from this node
	graph_analysis::EdgeIterator::Ptr edgeIterator = mPoseGraph->getEdgeIterator(vertex);
	std::set<graph_analysis::Vertex::Ptr> previously_matched_vertices;
	previously_matched_vertices.insert(vertex);
	while(edgeIterator->next())
	{
		EdgeObject::Ptr edge = boost::dynamic_pointer_cast<EdgeObject>(edgeIterator->current());
		if(edge->sensor == sensor->getName())
		{
			if(edge->getSourceVertex() == vertex)
				previously_matched_vertices.insert(edge->getTargetVertex());
			else
				previously_matched_vertices.insert(edge->getSourceVertex());
		}else
		{
			mLogger->message(WARNING, (boost::format("Ignoring edge from '%1%' while linking neighbors.") % edge->sensor).str());
		}
	}
	
	VertexList neighbors = getNearbyVertices(vertex->corrected_pose, mNeighborRadius);
	mLogger->message(DEBUG, (boost::format("Neighbor search found %1% vertices nearby.") % neighbors.size()).str());
	
	int added = 0;
	for(VertexList::iterator it = neighbors.begin(); it < neighbors.end() && added < max_links; it++)
	{
		if(previously_matched_vertices.find(*it) != previously_matched_vertices.end())
			continue;

		try
		{			
			Transform guess = (*it)->corrected_pose.inverse() * vertex->corrected_pose;
			TransformWithCovariance twc = sensor->calculateTransform((*it)->measurement, vertex->measurement, guess);
			addEdge(*it, vertex, twc.transform, twc.covariance, sensor->getName(), "match");
			added++;
		}catch(NoMatch &e)
		{
			continue;
		}
	}
}

VertexList GraphMapper::getVerticesFromSensor(const std::string& sensor)
{
	VertexList vertexList;

	graph_analysis::VertexIterator::Ptr vertexIterator = mPoseGraph->getVertexIterator();
	while(vertexIterator->next())
	{
		VertexObject::Ptr vertex = boost::dynamic_pointer_cast<VertexObject>(vertexIterator->current());
		if(vertex->measurement->getSensorName() == sensor)
		{
			vertexList.push_back(vertex);
		}
	}
	return vertexList;
}

EdgeList GraphMapper::getEdgesFromSensor(const std::string& sensor)
{
	EdgeList edgeList;
	
	graph_analysis::EdgeIterator::Ptr edgeIterator = mPoseGraph->getEdgeIterator();
	while(edgeIterator->next())
	{
		EdgeObject::Ptr edge = boost::dynamic_pointer_cast<EdgeObject>(edgeIterator->current());
		edgeList.push_back(edge);
	}
	
	return edgeList;
}

void GraphMapper::buildNeighborIndex(const std::string& sensor)
{
	VertexList vertices = getVerticesFromSensor(sensor);
	int numOfVertices = vertices.size();
	flann::Matrix<float> points(new float[numOfVertices * 3], numOfVertices, 3);

	int row = 0;
	mIndexMap.clear();
	for(VertexList::iterator it = vertices.begin(); it < vertices.end(); it++)
	{
		Transform::TranslationPart t = (*it)->corrected_pose.translation();
		points[row][0] = t[0];
		points[row][1] = t[1];
		points[row][2] = t[2];
		mIndexMap.insert(std::pair<int, VertexObject::Ptr>(row, *it));
		row++;
	}
	
	mIndex.buildIndex(points);
}

VertexList GraphMapper::getNearbyVertices(const Transform &tf, float radius)
{
	// Fill in the query point
	flann::Matrix<float> query(new float[3], 1, 3);
	Transform::ConstTranslationPart t = tf.translation();
	query[0][0] = t[0];
	query[0][1] = t[1];
	query[0][2] = t[2];
	
	// Find points nearby
	std::vector< std::vector<int> > neighbors;
	std::vector< std::vector<NeighborIndex::DistanceType> > distances;
	mIndex.radiusSearch(query, neighbors, distances, radius, mSearchParams);
	
	// Write the result
	VertexList result;
	std::vector<int>::iterator it;
	for(it = neighbors[0].begin(); it < neighbors[0].end(); it++)
	{
		result.push_back(mIndexMap[*it]);
	}
	
	return result;
}

Transform GraphMapper::getCurrentPose()
{
	return mCurrentPose;
}

void GraphMapper::writeGraphToFile(const std::string &name)
{
	std::string file = name + ".dot";
	mLogger->message(INFO, (boost::format("Writing graph to file '%1%'.") % file).str());
	graph_analysis::io::GraphIO::write(file, *mPoseGraph, graph_analysis::representation::GRAPHVIZ);
}

bool GraphMapper::checkMinDistance(const Transform &t)
{
	ScalarType rot = Eigen::AngleAxis<ScalarType>(t.rotation()).angle();
	if(rot > PI)
		rot = (2*PI) - rot;
	if(rot < -PI)
		rot += 2*PI;
	ScalarType dx = t.translation()(0);
	ScalarType dy = t.translation()(1);
	ScalarType dz = t.translation()(2);
	ScalarType trans = sqrt(dx*dx + dy*dy + dz*dz);
	mLogger->message(DEBUG, (boost::format("Translation: %1% / Rotation: %2%") % trans % rot).str());
	if(trans < mMinTranslation && rot < mMinRotation)
		return false;
	else
		return true;
}
