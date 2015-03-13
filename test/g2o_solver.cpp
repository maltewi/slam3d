#include <G2oSolver.hpp>

#include <iostream>

int main()
{
	slam::Logger logger;
	slam::G2oSolver* solver = new slam::G2oSolver(&logger);
	slam::VertexObject v1, v2, v3;
	v1.corrected_pose = Eigen::Translation<double, 3>(0,0,0);
	v2.corrected_pose = Eigen::Translation<double, 3>(0,0,0);
	v3.corrected_pose = Eigen::Translation<double, 3>(0,0,0);

	slam::EdgeObject e1,e2,e3;
	e1.transform = Eigen::Translation<double, 3>(1,0,0);
	e2.transform = Eigen::Translation<double, 3>(0,1,0);
	e3.transform = Eigen::Translation<double, 3>(-0.8, -0.7, 0.1);
	e1.covariance = slam::Covariance::Identity();
	e2.covariance = slam::Covariance::Identity();
	e3.covariance = slam::Covariance::Identity();
	
	solver->addNode(v1, 0);
	solver->addNode(v2, 1);
	solver->addNode(v3, 2);
	
	solver->addConstraint(e1, 0,1);
	solver->addConstraint(e2, 1,2);
	solver->addConstraint(e3, 2,0);
	
	solver->saveGraph("dummy.g2o");
	solver->compute();
	slam::IdPoseVector corr = solver->getCorrections();
	std::cout << "Results:" << std::endl;
	for(slam::IdPoseVector::iterator c = corr.begin(); c < corr.end(); c++)
	{
		std::cout << "Vertex " << c->first << ": Correction = ("
		<< c->second.translation()[0] << ","
		<< c->second.translation()[1] << ","
		<< c->second.translation()[2] << ")"<< std::endl;
	}
	return 0;
}