#define BOOST_TEST_MODULE "BoostGraphTest"

#include "BoostGraph.hpp"

#include <slam3d/core/FileLogger.hpp>
#include <slam3d/core/GraphTest.hpp>

using namespace slam3d;

BOOST_AUTO_TEST_CASE(boost_graph_construction)
{
	Clock clock;
	FileLogger logger(clock, "boost_graph.log");
	logger.setLogLevel(DEBUG);
	Graph* graph = new BoostGraph(&logger);
	test_graph_construction(graph);
	delete graph;
}
