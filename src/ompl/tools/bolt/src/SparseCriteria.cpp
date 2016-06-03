/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016, University of Colorado, Boulder
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Univ of CO, Boulder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Dave Coleman <dave@dav.ee>
   Desc:   Various tests to determine if a vertex/edge should be added to the graph, based on SPARSi
*/

// OMPL
#include <ompl/tools/bolt/SparseCriteria.h>

// Boost
#include <boost/foreach.hpp>

// Profiling
#include <valgrind/callgrind.h>

#define foreach BOOST_FOREACH

namespace og = ompl::geometric;
namespace ot = ompl::tools;
namespace otb = ompl::tools::bolt;
namespace ob = ompl::base;

namespace ompl
{
namespace tools
{
namespace bolt
{
SparseCriteria::SparseCriteria(SparseGraphPtr sg)
  : sg_(sg)
{
  // Copy the pointers of various components
  denseCache_ = sg->getDenseCache();
  si_ = sg->getSpaceInformation();
  visual_ = sg->getVisual();

  // Initialize discretizer
  vertexDiscretizer_.reset(new VertexDiscretizer(si_, visual_));
}

SparseCriteria::~SparseCriteria(void)
{
  clearanceSampler_.reset();
  regularSampler_.reset();
}

bool SparseCriteria::setup()
{
  const std::size_t indent = 0;

  // Dimensions / joints
  std::size_t dim = si_->getStateDimension();

  // Max distance across configuration space
  maxExtent_ = si_->getMaximumExtent();

  // Vertex visibility region size
  sparseDelta_ = sparseDeltaFraction_ * maxExtent_;

  // Sampling for interfaces visibility size
  denseDelta_ = denseDeltaFraction_ * maxExtent_;

  // Number of points to test for interfaces around a sample for the quality criterion
  nearSamplePoints_ = nearSamplePointsMultiple_ * si_->getStateDimension();

  // Discretization for initial input into sparse graph
  const double discFactor = sparseDelta_ - discretizePenetrationDist_;
  discretization_ = 2 * sqrt(std::pow(discFactor, 2) / dim);

  ignoreEdgesSmallerThan_ = (discretization_ + 0.01);

  // Calculate optimum stretch factor
  if (stretchFactor_ < std::numeric_limits<double>::epsilon())  // if stretchFactor is zero, auto set it
  {
    BOLT_DEBUG(indent, 1, "Auto settings stretch factor because input value was 0");
    // stretchFactor_ = discretization_ / (0.5 * discretization_ * sqrt(2) - 2.0 * denseDelta_);
    nearestDiscretizedV_ = sqrt(dim * std::pow(0.5 * discretization_, 2));  // z in my calculations
    // stretchFactor_ = 2.0 * discretization_ / ( nearestDiscretizedV_ - 2.0 * denseDelta_) + stretchFactor_; // 2D case
    // but not 3D
    stretchFactor_ =
        2.0 * discretization_ / (nearestDiscretizedV_) + stretchFactor_;  // 2D case without estimated interface amount
    // stretchFactor_ = (discretization_ + nearestDiscretizedV_) / ( 2 * (nearestDiscretizedV_ - 2.0 * denseDelta_)); //
    // N-D case
    // stretchFactor_ = discretization_ / (discretization_ - 2.0 * denseDelta_); // N-D case
  }

  BOLT_DEBUG(indent, 1, "--------------------------------------------------");
  BOLT_DEBUG(indent, 1, "Sparse DB Setup:");
  BOLT_DEBUG(indent + 2, 1, "Max Extent              = " << maxExtent_);
  BOLT_DEBUG(indent + 2, 1, "Sparse Delta            = " << sparseDelta_);
  BOLT_DEBUG(indent + 2, 1, "Dense Delta             = " << denseDelta_);
  BOLT_DEBUG(indent + 2, 1, "State Dimension         = " << dim);
  BOLT_DEBUG(indent + 2, 1, "Near Sample Points      = " << nearSamplePoints_);
  BOLT_DEBUG(indent + 2, 1, "Discretization          = " << discretization_);
  BOLT_DEBUG(indent + 2, 1, "Nearest Discretized V   = " << nearestDiscretizedV_);
  BOLT_DEBUG(indent + 2, 1, "Stretch Factor          = " << stretchFactor_);
  BOLT_DEBUG(indent + 2, 1, "Viz ignore edges below  = " << ignoreEdgesSmallerThan_);
  BOLT_DEBUG(indent, 1, "--------------------------------------------------");

  assert(maxExtent_ > 0);
  assert(denseDelta_ > 0);
  assert(nearSamplePoints_ > 0);
  assert(sparseDelta_ > 0);
  assert(sparseDelta_ > 0.000000001);  // Sanity check

  // Load minimum clearance state sampler
  if (!clearanceSampler_)
  {
    clearanceSampler_ = ob::MinimumClearanceValidStateSamplerPtr(new ob::MinimumClearanceValidStateSampler(si_.get()));
    clearanceSampler_->setMinimumObstacleClearance(obstacleClearance_);
    si_->getStateValidityChecker()->setClearanceSearchDistance(obstacleClearance_);
  }

  // Load regular state sampler
  if (!regularSampler_)
  {
    regularSampler_ = si_->allocValidStateSampler();
  }

  if (si_->getStateValidityChecker()->getClearanceSearchDistance() < obstacleClearance_)
    OMPL_WARN("State validity checker clearance search distance %f is less than the required obstacle clearance %f for "
              "our state sampler, incompatible settings!",
              si_->getStateValidityChecker()->getClearanceSearchDistance(), obstacleClearance_);

  // Configure vertex discretizer
  vertexDiscretizer_->setMinimumObstacleClearance(obstacleClearance_);
  vertexDiscretizer_->setDiscretization(discretization_);

  return true;
}

void SparseCriteria::createSPARS()
{
  std::size_t indent = 0;
  BOLT_BLUE_DEBUG(indent, true, "createSPARS()");
  indent += 2;

  // Error check
  if (!useRandomSamples_ && !useDiscretizedSamples_)
  {
    OMPL_WARN("Unable to create SPARS because both random sampling and discretized sampling is disabled");
    return;
  }

  // Benchmark runtime
  time::point startTime = time::now();

  numVerticesMoved_ = 0; // TODO move to SparseGraph?

  numConsecutiveFailures_ = 0;
  useFourthCriteria_ = false;  // initially we do not do this step

  // Profiler
  CALLGRIND_TOGGLE_COLLECT;

  // Start the graph off with discretized states
  if (useDiscretizedSamples_)
  {
    addDiscretizedStates(indent);
  }

  // Finish the graph with random samples
  if (useRandomSamples_)
  {
    addRandomSamples(indent);
  }

  if (!sg_->visualizeSparsGraph_)
    sg_->displayDatabase(true, indent);

  // Cleanup removed vertices
  sg_->removeDeletedVertices(indent);

  // Profiler
  CALLGRIND_TOGGLE_COLLECT;
  CALLGRIND_DUMP_STATS;

  // Benchmark runtime
  double duration = time::seconds(time::now() - startTime);

  // Statistics
  numGraphGenerations_++;

  // Check how many connected components exist, possibly throw error
  std::size_t numSets = sg_->getDisjointSetsCount();
  std::pair<std::size_t, std::size_t> interfaceStats = getInterfaceStateStorageSize();

  BOLT_DEBUG(indent, 1, "-----------------------------------------");
  BOLT_DEBUG(indent, 1, "Created SPARS graph                      ");
  BOLT_DEBUG(indent, 1, "  Vertices:                  " << sg_->getNumVertices());
  BOLT_DEBUG(indent, 1, "  Edges:                     " << sg_->getNumEdges());
  BOLT_DEBUG(indent, 1, "  Generation time:           " << duration);
  BOLT_DEBUG(indent, 1, "  Total generations:         " << numGraphGenerations_);
  BOLT_DEBUG(indent, 1, "  Disjoint sets:             " << numSets);
  BOLT_DEBUG(indent, 1, "  DenseCache                 ");
  BOLT_DEBUG(indent, 1, "    Edge cache         ");
  BOLT_DEBUG(indent, 1, "      Size:                    " << denseCache_->getEdgeCacheSize());
  BOLT_DEBUG(indent, 1, "      Total checks:            " << denseCache_->getTotalCollisionChecks());
  BOLT_DEBUG(indent, 1, "      Cached checks:           " << denseCache_->getTotalCollisionChecksFromCache() << " ("
                                                          << denseCache_->getPercentCachedCollisionChecks() << "%)");
  BOLT_DEBUG(indent, 1, "    State cache                ");
  BOLT_DEBUG(indent, 1, "      Size:                    " << denseCache_->getStateCacheSize());
  BOLT_DEBUG(indent, 1, "  Criteria additions:        ");
  BOLT_DEBUG(indent, 1, "    Coverage:                " << sg_->numSamplesAddedForCoverage_);
  BOLT_DEBUG(indent, 1, "    Connectivity:            " << sg_->numSamplesAddedForConnectivity_);
  BOLT_DEBUG(indent, 1, "    Interface:               " << sg_->numSamplesAddedForInterface_);
  BOLT_DEBUG(indent, 1, "    Quality:                 " << sg_->numSamplesAddedForQuality_);
  BOLT_DEBUG(indent, 1, "  Num random samples added:  " << numRandSamplesAdded_);
  BOLT_DEBUG(indent, 1, "  Num vertices moved:        " << numVerticesMoved_);
  BOLT_DEBUG(indent, 1, "  InterfaceData:             ");
  BOLT_DEBUG(indent, 1, "    States stored:           " << interfaceStats.first);
  BOLT_DEBUG(indent, 1, "    Missing interfaces:      " << interfaceStats.second);
  BOLT_DEBUG(indent, 1, "-----------------------------------------");

  if (!sg_->visualizeSparsGraph_)
    sg_->displayDatabase(true, indent);

  OMPL_INFORM("Finished creating sparse database");
}

void SparseCriteria::addDiscretizedStates(std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, true, "addDiscretizedStates()");
  indent += 2;

  // This only runs if the graph is empty
  if (!sg_->isEmpty())
  {
    BOLT_RED_DEBUG(indent, true, "Unable to generate discretized states because graph is not empty");
    return;
  }

  // Generate discretization
  vertexDiscretizer_->generateLattice(indent);

  // this tells SPARS to always add the vertex, no matter what
  setDiscretizedSamplesInsertion(true);

  // Convert to proper format
  std::vector<base::State *> &candidateVertices = vertexDiscretizer_->getCandidateVertices();
  std::list<WeightedVertex> vertexInsertionOrder;

  for (base::State *state : candidateVertices)
  {
    if (false)
      sg_->debugState(state);

    // Move the ompl::base::State to the DenseCache, changing its ownership
    StateID candidateStateID = denseCache_->addState(state);
    vertexInsertionOrder.push_back(WeightedVertex(candidateStateID, 0));
  }
  candidateVertices.clear();  // clear the vector because we've moved all its memory pointers to DenseCache
  std::size_t sucessfulInsertions;
  createSPARSInnerLoop(vertexInsertionOrder, sucessfulInsertions);

  setDiscretizedSamplesInsertion(false);

  // Make sure discretization doesn't have any bugs
  if (sg_->superDebug_)
    sg_->errorCheckDuplicateStates(indent);
}

/*
void SparseCriteria::createSPARSOuterLoop()
{
  std::size_t indent = 2;

  // Reset parameters
  setup();
  visualizeOverlayNodes_ = false;  // DO NOT visualize all added nodes in a separate window
  denseCache_->resetCounters();

  // Get the ordering to insert vertices
  std::list<WeightedVertex> vertexInsertionOrder;
  getVertexInsertionOrdering(vertexInsertionOrder);

  // Error check order creation
  assert(vertexInsertionOrder.size() == getNumVertices() - queryVertices_.size());

  // Attempt to insert the vertices multiple times until no more succesful insertions occur
  secondSparseInsertionAttempt_ = false;
  std::size_t loopAttempt = 0;
  std::size_t sucessfulInsertions = 1;  // start with one so that while loop works
  while (sucessfulInsertions > 0)
  {
    std::cout << "Attempting to insert " << vertexInsertionOrder.size() << " vertices for the " << loopAttempt
              << " loop" << std::endl;

    // Sanity check
    if (loopAttempt > 3)
      OMPL_WARN("Suprising number of loop when attempting to insert nodes into SPARS graph: %u", loopAttempt);

    // Benchmark runtime
    time::point startTime = time::now();

    // ----------------------------------------------------------------------
    // Attempt to insert each vertex using the first 3 criteria
    if (!createSPARSInnerLoop(vertexInsertionOrder, sucessfulInsertions))
      break;

    // Benchmark runtime
    double duration = time::seconds(time::now() - startTime);

    // Visualize
    if (visualizeSparsGraph_)
      visual_->viz1Trigger();

    std::cout << "Succeeded in inserting " << sucessfulInsertions << " vertices on the " << loopAttempt
              << " loop, remaining uninserted vertices: " << vertexInsertionOrder.size()
              << " loop runtime: " << duration << " sec" << std::endl;
    loopAttempt++;

    // Increase the sparse delta a bit, but only after the first loop
    if (loopAttempt == 1)
    {
      // sparseDelta_ = getSecondarySparseDelta();
      std::cout << std::string(indent + 2, ' ') << "sparseDelta_ is now " << sparseDelta_ << std::endl;
      secondSparseInsertionAttempt_ = true;

      // Save collision cache, just in case there is a bug
      denseCache_->save();
    }

    bool debugOverRideJustTwice = true;
    if (debugOverRideJustTwice && loopAttempt == 1)
    {
      OMPL_WARN("Only attempting to add nodes twice for speed");
      break;
    }
  }

  // If we haven't animated the creation, just show it all at once
  if (!visualizeSparsGraph_)
  {
    sg_->displayDatabase(true, indent+4);
  }
  else if (sg_->visualizeSparsGraphSpeed_ < std::numeric_limits<double>::epsilon())
  {
    visual_->viz1Trigger();
    usleep(0.001 * 1000000);
  }
}
*/

bool SparseCriteria::createSPARSInnerLoop(std::list<WeightedVertex> &vertexInsertionOrder, std::size_t &sucessfulInsertions)
{
  std::size_t indent = 0;

  sucessfulInsertions = 0;
  std::size_t loopCount = 0;
  std::size_t originalVertexInsertion = vertexInsertionOrder.size();
  std::size_t debugFrequency = std::max(std::size_t(10), static_cast<std::size_t>(originalVertexInsertion / 20));

  for (std::list<WeightedVertex>::iterator vertexIt = vertexInsertionOrder.begin();
       vertexIt != vertexInsertionOrder.end();
       /* will manually progress since there are erases*/)
  {
    // User feedback
    if (loopCount++ % debugFrequency == 0)
    {
      std::cout << ANSI_COLOR_BLUE;
      std::cout << std::fixed << std::setprecision(1)
                << "Sparse generation progress: " << (static_cast<double>(loopCount) / originalVertexInsertion) * 100.0
                << "% Cache size: " << denseCache_->getEdgeCacheSize()
                << " Cache usage: " << denseCache_->getPercentCachedCollisionChecks() << "%" << std::endl;
      std::cout << ANSI_COLOR_RESET;
      if (sg_->visualizeSparsGraph_)
        visual_->viz1Trigger();
    }

    // Run SPARS checks
    VertexType addReason;    // returns why the state was added
    SparseVertex newVertex;  // the newly generated sparse vertex

    // TODO(davetcoleman): I would like to multi-thread this but its not worth my time currently
    std::size_t threadID = 0;
    if (!addStateToRoadmap(vertexIt->stateID_, newVertex, addReason, threadID, indent))
    {
      // std::cout << "Failed AGAIN to add state to roadmap------" << std::endl;

      // Visualize the failed vertex as a small red dot
      if (sg_->visualizeSparsGraph_ && false)
      {
        visual_->viz1State(denseCache_->getState(vertexIt->stateID_), tools::SMALL, tools::RED, 0);
      }
      vertexIt++;  // increment since we didn't remove anything from the list
    }
    else
    {
      // If a new vertex was created, update its popularity property
      if (newVertex)  // value is not null, so it was created
      {
        // std::cout << "SETTING POPULARITY of vertex " << newVertex << " to value "
        //<< vertexInsertionOrder[i].weight_ << std::endl;

        // Update popularity
        //vertexPopularity_[newVertex] = vertexIt->weight_;
      }

      // Remove this state from the candidates for insertion vector
      vertexIt = vertexInsertionOrder.erase(vertexIt);

      sucessfulInsertions++;
    }
  }  // end for

  return true;
}

void SparseCriteria::addRandomSamples(std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "addRandomSamples() ==================================================");
  indent += 2;

  // Clear stats
  numRandSamplesAdded_ = 0;

  while (true)
  {
    base::State *candidateState = si_->allocState();
    StateID candidateStateID = denseCache_->addState(candidateState);

    // Sample randomly
    if (!clearanceSampler_->sample(candidateState))
    {
      OMPL_ERROR("Unable to find valid sample");
      exit(-1);  // this should never happen
    }
    // si_->getStateSpace()->setLevel(candidateState, 0);  // TODO no hardcode

    // Debug
    if (false)
    {
      BOLT_DEBUG(indent, vCriteria_, "Randomly sampled stateID: " << candidateStateID);
      sg_->debugState(candidateState);
    }

    if (!addSample(candidateStateID, indent))
      break;
  }  // end while
}

void SparseCriteria::addSamplesFromCache(std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "addRandomSamplesFromCache() ==================================================");
  indent += 2;

  if (denseCache_->getStateCacheSize() <= 1)
  {
    BOLT_YELLOW_DEBUG(indent, true, "Cache is empty, no states to add");
    return;
  }

  if (useDiscretizedSamples_)
  {
    BOLT_YELLOW_DEBUG(indent, true, "Not using cache for random samples because discretized samples is enabled.");
    return;
  }

  // Add from file if available - remember where it was
  std::size_t lastCachedStateIndex = denseCache_->getStateCacheSize() - 1;
  StateID candidateStateID = 1;  // skip 0, because that is the "deleted" NULL state ID

  while (candidateStateID <= lastCachedStateIndex)
  {
    if (!addSample(candidateStateID, indent))
      break;

    candidateStateID++;
  }  // end while
}

bool SparseCriteria::addSample(StateID candidateStateID, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "addSample()");
  indent += 2;

  // Run SPARS checks
  VertexType addReason;    // returns why the state was added
  SparseVertex newVertex;  // the newly generated sparse vertex
  const std::size_t threadID = 0;
  if (addStateToRoadmap(candidateStateID, newVertex, addReason, threadID, indent))
  {
    // if (numRandSamplesAdded_ % 10 == 0)
    BOLT_DEBUG(indent, vCriteria_, "Added random sample with stateID "
                                       << candidateStateID << ", total new states: " << ++numRandSamplesAdded_);
  }
  else if (numConsecutiveFailures_ % 1000 == 0)
  {
    BOLT_DEBUG(indent, true, "Random sample failed, consecutive failures: " << numConsecutiveFailures_);
  }

  // Check consecutive failures
  if (numConsecutiveFailures_ >= fourthCriteriaAfterFailures_ && !useFourthCriteria_)
  {
    BOLT_YELLOW_DEBUG(indent, true, "Starting to check for 4th quality criteria because "
                                        << numConsecutiveFailures_ << " consecutive failures have occured");
    useFourthCriteria_ = true;
    visualizeOverlayNodes_ = true;  // visualize all added nodes in a separate window
    numConsecutiveFailures_ = 0;    // reset for new criteria

    // Show it just once if it has not already been animated
    if (!visualizeVoronoiDiagramAnimated_ && visualizeVoronoiDiagram_)
      visual_->vizVoronoiDiagram();
  }

  if (useFourthCriteria_ && numConsecutiveFailures_ > terminateAfterFailures_)
  {
    BOLT_YELLOW_DEBUG(indent, true, "SPARS creation finished because " << terminateAfterFailures_
                                                                       << " consecutive insertion failures reached");
    return false;  // stop inserting states
  }
  return true;  // continue going
}

/*
void SparseCriteria::getVertexInsertionOrdering(std::list<WeightedVertex> &vertexInsertionOrder)
{
  if (sparseCreationInsertionOrder_ == 0)
  {
    OMPL_INFORM("Creating sparse graph using popularity ordering");
    getPopularityOrder(vertexInsertionOrder);  // Create SPARs graph in order of popularity
  }
  else if (sparseCreationInsertionOrder_ == 1)
  {
    OMPL_WARN("Creating sparse graph using default ordering");
    getDefaultOrder(vertexInsertionOrder);
  }
  else if (sparseCreationInsertionOrder_ == 2)
  {
    OMPL_WARN("Creating sparse graph using random ordering");
    getRandomOrder(vertexInsertionOrder);
  }
  else
  {
    OMPL_ERROR("Unknown insertion order method");
    exit(-1);
  }
}

bool SparseCriteria::getPopularityOrder(std::list<WeightedVertex> &vertexInsertionOrder)
{
  bool verbose = false;

  // Error check
  BOOST_ASSERT_MSG(getNumVertices() > queryVertices_.size(),
                   "Unable to get vertices in order of popularity because dense "
                   "graph is empty");

  if (visualizeNodePopularity_)  // Clear visualization
  {
    visual_->viz3DeleteAllMarkers();
  }

  // Sort the vertices by popularity in a queue
  std::priority_queue<WeightedVertex, std::vector<WeightedVertex>, CompareWeightedVertex> pqueue;

  // Loop through each popular edge in the dense graph
  foreach (TaskVertex v, boost::vertices(g_))
  {
    // Do not process the search vertex, it is null
    if (v <= queryVertices_.back())
      continue;

    if (verbose)
      std::cout << "Vertex: " << v << std::endl;
    double popularity = 0;
    foreach (TaskVertex edge, boost::out_edges(v, g_))
    {
      if (verbose)
        std::cout << "  Edge: " << edge << std::endl;
      popularity += (100 - edgeWeightProperty_[edge]);
    }
    if (verbose)
      std::cout << "  Total popularity: " << popularity << std::endl;
    pqueue.push(WeightedVertex(v, popularity));
  }

  // Remember which one was the largest
  double largestWeight = pqueue.top().weight_;
  if (largestWeight == 0)
    largestWeight = 1;  // prevent division by zero

  if (verbose)
    std::cout << "Largest weight: " << largestWeight << std::endl
              << std::endl;

  // Convert pqueue into vector
  while (!pqueue.empty())  // Output the vertices in order
  {
    vertexInsertionOrder.push_back(pqueue.top());

    // Modify the weight to be a percentage of the max weight
    const double weightPercent = pqueue.top().weight_ / largestWeight * 100.0;
    vertexInsertionOrder.back().weight_ = weightPercent;

    // Visualize
    if (visualizeNodePopularity_)
    {
      if (verbose)
        std::cout << "vertex " << pqueue.top().v_ << ", mode 7, weightPercent " << weightPercent << std::endl;
      visual_->viz3State(stateProperty_[pqueue.top().v_], tools::SCALE, tools::BLACK, weightPercent);
    }

    // Remove from priority queue
    pqueue.pop();
  }

  if (visualizeNodePopularity_)
  {
    visual_->viz3Trigger();
    usleep(0.001 * 1000000);
  }

  return true;
}

bool SparseCriteria::getDefaultOrder(std::list<WeightedVertex> &vertexInsertionOrder)
{
  bool verbose = false;
  double largestWeight = -1 * std::numeric_limits<double>::infinity();

  // Loop through each popular edge in the dense graph
  foreach (TaskVertex v, boost::vertices(g_))
  {
    // Do not process the search vertex, it is null
    if (v <= queryVertices_.back())
      continue;

    if (verbose)
      std::cout << "Vertex: " << v << std::endl;
    double popularity = 0;

    foreach (TaskVertex edge, boost::out_edges(v, g_))
    {
      if (verbose)
        std::cout << "  Edge: " << edge << std::endl;
      popularity += (100 - edgeWeightProperty_[edge]);
    }
    if (verbose)
      std::cout << "  Total popularity: " << popularity << std::endl;

    // Record
    vertexInsertionOrder.push_back(WeightedVertex(v, popularity));

    // Track largest weight
    if (popularity > largestWeight)
      largestWeight = popularity;
  }

  // Update the weights
  for (WeightedVertex wv : vertexInsertionOrder)
  {
    // Modify the weight to be a percentage of the max weight
    const double weightPercent = wv.weight_ / largestWeight * 100.0;
    wv.weight_ = weightPercent;

    // Visualize vertices
    if (visualizeNodePopularity_)
    {
      visual_->viz3State(stateProperty_[wv.v_], tools::SCALE, tools::BLACK, weightPercent);
    }
  }

  // Visualize vertices
  if (visualizeNodePopularity_)
  {
    visual_->viz3Trigger();
    usleep(0.001 * 1000000);
  }

  return true;
}

bool SparseCriteria::getRandomOrder(std::list<WeightedVertex> &vertexInsertionOrder)
{
  std::list<WeightedVertex> defaultVertexInsertionOrder;
  getDefaultOrder(defaultVertexInsertionOrder);

  // Create vector and copy list into it
  std::vector<WeightedVertex> tempVector(defaultVertexInsertionOrder.size());
  std::copy(defaultVertexInsertionOrder.begin(), defaultVertexInsertionOrder.end(), tempVector.begin());

  // Randomize
  std::random_shuffle(tempVector.begin(), tempVector.end());

  // Convert back to list
  vertexInsertionOrder.resize(tempVector.size());
  std::copy(tempVector.begin(), tempVector.end(), vertexInsertionOrder.begin());

  return true;
}
*/

bool SparseCriteria::addStateToRoadmap(StateID candidateStateID, SparseVertex &newVertex, VertexType &addReason,
                                 std::size_t threadID, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "addStateToRoadmap() Adding candidate state ID " << candidateStateID);

  if (visualizeAttemptedStates_)
  {
    visual_->viz2DeleteAllMarkers();
    visual_->viz2State(sg_->getState(candidateStateID), tools::LARGE, tools::GREEN, 0);
    visual_->viz2Trigger();
    usleep(0.001 * 1000000);
  }

  bool stateAdded = false;
  base::State *workState = si_->allocState();

  // Nodes near our input state
  std::vector<SparseVertex> graphNeighborhood;
  // Visible nodes near our input state
  std::vector<SparseVertex> visibleNeighborhood;

  // Find nearby nodes
  findGraphNeighbors(candidateStateID, graphNeighborhood, visibleNeighborhood, threadID, indent + 2);

  // Always add a node if no other nodes around it are visible (GUARD)
  if (checkAddCoverage(candidateStateID, visibleNeighborhood, newVertex, indent + 2))
  {
    BOLT_DEBUG(indent + 2, vAddedReason_, "Graph updated for: COVERAGE, stateID: " << candidateStateID);

    addReason = COVERAGE;
    stateAdded = true;
  }
  else if (checkAddConnectivity(candidateStateID, visibleNeighborhood, newVertex, indent + 6))
  {
    BOLT_DEBUG(indent + 2, vAddedReason_, "Graph updated for: CONNECTIVITY, stateID: " << candidateStateID);

    addReason = CONNECTIVITY;
    stateAdded = true;
  }
  else if (checkAddInterface(candidateStateID, graphNeighborhood, visibleNeighborhood, newVertex, indent + 10))
  {
    BOLT_DEBUG(indent + 2, vAddedReason_, "Graph updated for: INTERFACE, stateID: " << candidateStateID);

    addReason = INTERFACE;
    stateAdded = true;
  }
  else if (useFourthCriteria_ &&
           checkAddQuality(candidateStateID, graphNeighborhood, visibleNeighborhood, workState, newVertex, indent + 14))
  {
    BOLT_DEBUG(indent + 2, vAddedReason_, "Graph updated for: QUALITY, stateID: " << candidateStateID);

    addReason = QUALITY;
    stateAdded = true;
  }
  else if (discretizedSamplesInsertion_)
  {
    BOLT_DEBUG(indent + 2, vAddedReason_, "Graph updated for: DISCRETIZED, stateID: " << candidateStateID);
    newVertex = sg_->addVertex(candidateStateID, DISCRETIZED, indent + 2);

    addReason = DISCRETIZED;
    stateAdded = true;
  }
  else
  {
    BOLT_DEBUG(indent + 2, vCriteria_, "Did NOT add state for any criteria ");
    numConsecutiveFailures_++;
  }

  if (stateAdded)
    numConsecutiveFailures_ = 0;

  si_->freeState(workState);

  return stateAdded;
}

bool SparseCriteria::checkAddCoverage(StateID candidateStateID, std::vector<SparseVertex> &visibleNeighborhood,
                                SparseVertex &newVertex, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "checkAddCoverage() Are other nodes around it visible?");

  // Only add a node for coverage if it has no neighbors
  if (visibleNeighborhood.size() > 0)
  {
    BOLT_DEBUG(indent + 2, vCriteria_, "NOT adding node for coverage ");
    return false;  // has visible neighbors
  }

  // No free paths means we add for coverage
  BOLT_DEBUG(indent + 2, vCriteria_, "Adding node for COVERAGE ");

  newVertex = sg_->addVertex(candidateStateID, COVERAGE, indent + 4);

  // Note: we do not connect this node with any edges because we have already determined
  // it is too far away from any nearby nodes

  return true;
}

bool SparseCriteria::checkAddConnectivity(StateID candidateStateID, std::vector<SparseVertex> &visibleNeighborhood,
                                    SparseVertex &newVertex, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "checkAddConnectivity() Does this node connect two disconnected components?");

  // If less than 2 neighbors there is no way to find a pair of nodes in different connected components
  if (visibleNeighborhood.size() < 2)
  {
    BOLT_DEBUG(indent + 2, vCriteria_, "NOT adding node for connectivity");
    return false;
  }

  // Identify visibile nodes around our new state that are unconnected (in different connected components)
  // and connect them
  std::set<SparseVertex> statesInDiffConnectedComponents;

  // For each neighbor
  for (std::size_t i = 0; i < visibleNeighborhood.size(); ++i)
  {
    // For each other neighbor
    for (std::size_t j = i + 1; j < visibleNeighborhood.size(); ++j)
    {
      // If they are in different components
      if (!sg_->sameComponent(visibleNeighborhood[i], visibleNeighborhood[j]))
      {
        BOLT_DEBUG(indent + 2, vCriteria_, "Different connected component: " << visibleNeighborhood[i] << ", "
                                                                             << visibleNeighborhood[j]);

        if (visualizeConnectivity_)
        {
          visual_->viz2State(sg_->getVertexState(visibleNeighborhood[i]), tools::MEDIUM, tools::BLUE, 0);
          visual_->viz2State(sg_->getVertexState(visibleNeighborhood[j]), tools::MEDIUM, tools::BLUE, 0);
          visual_->viz2Trigger();
          usleep(0.001 * 1000000);
        }

        statesInDiffConnectedComponents.insert(visibleNeighborhood[i]);
        statesInDiffConnectedComponents.insert(visibleNeighborhood[j]);
      }
    }
  }

  // Were any disconnected states found?
  if (statesInDiffConnectedComponents.empty())
  {
    BOLT_DEBUG(indent + 2, vCriteria_, "NOT adding node for connectivity");
    return false;
  }

  BOLT_DEBUG(indent + 2, vCriteria_, "Adding node for CONNECTIVITY ");

  // Add the node
  newVertex = sg_->addVertex(candidateStateID, CONNECTIVITY, indent + 4);

  // Check if there are really close vertices nearby which should be merged
  checkRemoveCloseVertices(newVertex, indent + 4);

  // Add the edges
  for (std::set<SparseVertex>::const_iterator vertexIt = statesInDiffConnectedComponents.begin();
       vertexIt != statesInDiffConnectedComponents.end(); ++vertexIt)
  {
    BOLT_DEBUG(indent + 4, vCriteria_, "Loop: Adding vertex " << *vertexIt);

    if (sg_->getStateID(*vertexIt) == 0)
    {
      BOLT_DEBUG(indent + 4, vCriteria_, "Skipping because vertex " << *vertexIt << " was removed (state marked as 0)");
      continue;
    }

    // Do not add edge from self to self
    if (si_->getStateSpace()->equalStates(sg_->getVertexState(*vertexIt), sg_->getVertexState(newVertex)))
    {
      BOLT_RED_DEBUG(indent + 4, 1, "Prevented same vertex from being added twice ");
      continue;  // skip this pairing
    }

    // New vertex should not be connected to anything - there's no edge between the two states
    if (sg_->hasEdge(newVertex, *vertexIt) == true)
    {
      BOLT_DEBUG(indent + 4, 1, "The new vertex " << newVertex << " is already connected to old vertex");
      continue;
    }

    // The components haven't been united by previous edges created in this for loop
    if (!sg_->sameComponent(*vertexIt, newVertex))
    {
      // Connect
      sg_->addEdge(newVertex, *vertexIt, eCONNECTIVITY, indent + 4);
    }
    else
    {
      // This is not a big deal
      // OMPL_WARN("Two states that where not prev in the same component were joined during the same for "
      //"loop");
    }
  }

  return true;
}

bool SparseCriteria::checkAddInterface(StateID candidateStateID, std::vector<SparseVertex> &graphNeighborhood,
                                 std::vector<SparseVertex> &visibleNeighborhood, SparseVertex &newVertex,
                                 std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "checkAddInterface() Does this node's neighbor's need it to better connect "
                                      "them?");
  indent += 2;

  // If there are less than two neighbors the interface property is not applicable, because requires
  // two closest visible neighbots
  if (visibleNeighborhood.size() < 2)
  {
    BOLT_DEBUG(indent, vCriteria_, "NOT adding node for interface (less than 2 visible neighbors)");
    return false;
  }

  // If the two closest nodes are also visible
  const std::size_t threadID = 0;
  if (graphNeighborhood[0] == visibleNeighborhood[0] && graphNeighborhood[1] == visibleNeighborhood[1])
  {
    // If our two closest neighbors don't share an edge
    if (!sg_->hasEdge(visibleNeighborhood[0], visibleNeighborhood[1]))
    {
      // If they can be directly connected
      if (denseCache_->checkMotionWithCacheVertex(visibleNeighborhood[0], visibleNeighborhood[1], threadID))
      {
        BOLT_DEBUG(indent, vCriteria_, "INTERFACE: directly connected nodes");

        SparseVertex v1 = visibleNeighborhood[0];
        SparseVertex v2 = visibleNeighborhood[1];
        if (si_->getStateSpace()->equalStates(sg_->getVertexState(v1), sg_->getVertexState(v2)))
        {
          OMPL_ERROR("States are equal");
          visualizeRemoveCloseVertices(v1, v2);

          std::cout << "v1: " << v1 << " stateID1: " << sg_->getStateID(v1) << " state address: " << sg_->getVertexState(v1)
                    << " state: ";
          sg_->debugState(sg_->getVertexState(v1));
          std::cout << "v2: " << v2 << " stateID2: " << sg_->getStateID(v2) << " state address: " << sg_->getVertexState(v2)
                    << " state: ";
          sg_->debugState(sg_->getVertexState(v2));

          denseCache_->print();
        }

        // Connect them
        sg_->addEdge(visibleNeighborhood[0], visibleNeighborhood[1], eINTERFACE, indent + 2);

        // Also add the vertex if we are in a special mode where we know its desired
        if (discretizedSamplesInsertion_)
          newVertex = sg_->addVertex(candidateStateID, DISCRETIZED, indent + 2);
      }
      else  // They cannot be directly connected
      {
        // Add the new node to the graph, to bridge the interface
        BOLT_DEBUG(indent, vCriteria_, "Adding node for INTERFACE");

        newVertex = sg_->addVertex(candidateStateID, INTERFACE, indent + 2);

        // Check if there are really close vertices nearby which should be merged
        if (checkRemoveCloseVertices(newVertex, indent + 2))
        {
          // New vertex replaced a nearby vertex, we can continue no further because graph has been re-indexed
          return true;
        }

        if (sg_->getVertexState(visibleNeighborhood[0]) == NULL)
        {
          BOLT_RED_DEBUG(indent + 3, 1, "Skipping edge 0 because vertex was removed");
          visual_->waitForUserFeedback("skipping edge 0");
        }
        else
          sg_->addEdge(newVertex, visibleNeighborhood[0], eINTERFACE, indent + 2);

        if (sg_->getVertexState(visibleNeighborhood[1]) == NULL)
        {
          BOLT_RED_DEBUG(indent + 3, 1, "Skipping edge 1 because vertex was removed");
          visual_->waitForUserFeedback("skipping edge 2");
        }
        else
          sg_->addEdge(newVertex, visibleNeighborhood[1], eINTERFACE, indent + 2);

        BOLT_DEBUG(indent, vCriteria_, "INTERFACE: connected two neighbors through new interface node");
      }

      // Report success
      return true;
    }
    else
    {
      BOLT_DEBUG(indent, vCriteria_, "Two closest two neighbors already share an edge, not connecting them");
    }
  }
  BOLT_DEBUG(indent, vCriteria_, "NOT adding node for interface");
  return false;
}

bool SparseCriteria::checkAddQuality(StateID candidateStateID, std::vector<SparseVertex> &graphNeighborhood,
                               std::vector<SparseVertex> &visibleNeighborhood, base::State *workState,
                               SparseVertex &newVertex, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "checkAddQuality() Ensure SPARS asymptotic optimality");
  indent += 2;

  if (visibleNeighborhood.empty())
  {
    BOLT_DEBUG(indent, vQuality_, "no visible neighbors, not adding 4th criteria ");
    return false;
  }

  base::State *candidateState = denseCache_->getStateNonConst(candidateStateID);  // paper's name: q
  SparseVertex candidateRep = visibleNeighborhood[0];                             // paper's name: v

  bool added = false;
  std::map<SparseVertex, base::State *> closeRepresentatives;  // [nearSampledRep, nearSampledState]
  findCloseRepresentatives(workState, candidateStateID, candidateRep, closeRepresentatives, indent + 2);

  BOLT_DEBUG(indent, vQuality_, "back in checkAddQuality(): Found " << closeRepresentatives.size()
                                                                    << " close representatives");

  bool updated = false;  // track whether a change was made to any vertices' representatives

  // For each pair of close representatives
  for (std::map<SparseVertex, base::State *>::iterator it = closeRepresentatives.begin();
       it != closeRepresentatives.end(); ++it)
  {
    BOLT_DEBUG(indent + 2, vQuality_, "Looping through close representatives");
    base::State *nearSampledState = it->second;  // paper: q'
    SparseVertex nearSampledRep = it->first;     // paper: v'

    if (visualizeQualityCriteria_ && false)  // Visualization
    {
      visual_->viz3Edge(sg_->getVertexState(nearSampledRep), nearSampledState, tools::eRED);

      visual_->viz3State(nearSampledState, tools::MEDIUM, tools::GREEN, 0);

      // Replicate a regular vertex visualization
      // visual_->viz3State(sg_->getVertexState(nearSampledRep), tools::VARIABLE_SIZE, tools::TRANSLUCENT_LIGHT,
      // sparseDelta_);
      visual_->viz3State(sg_->getVertexState(nearSampledRep), tools::LARGE, tools::PURPLE, sparseDelta_);

      visual_->viz3Trigger();
      usleep(0.001 * 1000000);
    }

    // Update interface bookkeeping
    // Process:
    // 1. Get adjacent vertieces of the candidateRep (v) that are unconnected to nearSampledRep (v')
    //      e.g. v''
    // 2. For every adj vertex that is unconnected to v'
    // 3. Check distance:
    //    3.1. Get the interface data stored on vertex candidateRep(v) for max distance between
    //           nearSampledRep (v') and adjVertexUnconnected (v'')
    //    3.2. Add the candidateState (q) and nearSampledState (q') as 'first'

    // Attempt to update bookkeeping for candidateRep (v)
    if (updatePairPoints(candidateRep, candidateState, nearSampledRep, nearSampledState, indent + 2))
      updated = true;

    // ALSO attempt to update bookkeeping for neighboring node nearSampleRep (v')
    if (updatePairPoints(nearSampledRep, nearSampledState, candidateRep, candidateState, indent + 2))
      updated = true;
  }

  BOLT_DEBUG(indent, vQuality_, "Done updating pair points");

  if (!updated)
  {
    BOLT_DEBUG(indent, vQuality_, "No representatives were updated, so not calling checkAddPath()");
    return false;
  }

  // Visualize the interfaces around the candidate rep
  if (visualizeQualityCriteria_)
  {
    // visualizeCheckAddQuality(candidateState, candidateRep);
    // visualizeInterfaces(candidateRep, indent + 2);

    // static std::size_t updateCount = 0;
    // if (updateCount++ % 50 == 0)
    //   visualizeAllInterfaces(indent + 2);
  }

  // Attempt to find shortest path through closest neighbour
  if (checkAddPath(candidateRep, indent + 2))
  {
    BOLT_DEBUG(indent, vQuality_, "nearest visible neighbor added for path ");
    added = true;
  }

  // Attempt to find shortest path through other pairs of representatives
  for (std::map<SparseVertex, base::State *>::iterator it = closeRepresentatives.begin();
       it != closeRepresentatives.end(); ++it)
  {
    BOLT_YELLOW_DEBUG(indent, vQuality_, "Looping through close representatives to add path ===============");

    base::State *nearSampledState = it->second;  // paper: q'
    SparseVertex nearSampledRep = it->first;     // paper: v'
    if (checkAddPath(nearSampledRep, indent + 2))
    {
      BOLT_DEBUG(indent, vQuality_, "Close representative added for path");
      added = true;
    }

    // Delete state that was allocated and sampled within this function
    si_->freeState(nearSampledState);
  }

  return added;
}

void SparseCriteria::visualizeCheckAddQuality(StateID candidateStateID, SparseVertex candidateRep)
{
  visual_->viz3DeleteAllMarkers();

  visual_->viz3Edge(denseCache_->getState(candidateStateID), sg_->getVertexState(candidateRep), tools::eORANGE);

  // Show candidate state
  // visual_->viz3State(denseCache_->getState(candidateStateID), tools::VARIABLE_SIZE, tools::TRANSLUCENT_LIGHT,
  // denseDelta_);
  visual_->viz3State(denseCache_->getState(candidateStateID), tools::LARGE, tools::RED, 0);

  // Show candidate state's representative
  visual_->viz3State(sg_->getVertexState(candidateRep), tools::LARGE, tools::BLUE, 0);

  // Show candidate state's representative's neighbors
  foreach (SparseVertex adjVertex, boost::adjacent_vertices(candidateRep, sg_->getGraph()))
  {
    visual_->viz3Edge(sg_->getVertexState(adjVertex), sg_->getVertexState(candidateRep), tools::eGREEN);
    visual_->viz3State(sg_->getVertexState(adjVertex), tools::LARGE, tools::PURPLE, 0);
  }

  visual_->viz3Trigger();
  usleep(0.001 * 1000000);
}

bool SparseCriteria::checkAddPath(SparseVertex v, std::size_t indent)
{
  BOLT_CYAN_DEBUG(indent, vQuality_, "checkAddPath() v = " << v);
  indent += 2;
  bool spannerPropertyWasViolated = false;

  // Candidate v" vertices as described in the method, filled by function getAdjVerticesOfV1UnconnectedToV2().
  std::vector<SparseVertex> adjVerticesUnconnected;

  // Copy adjacent vertices into vector because we might add additional edges during this function
  std::vector<SparseVertex> adjVertices;
  foreach (SparseVertex adjVertex, boost::adjacent_vertices(v, sg_->getGraph()))
    adjVertices.push_back(adjVertex);

  BOLT_DEBUG(indent, vQuality_, "Vertex v = " << v << " has " << adjVertices.size() << " adjacent vertices, "
                                                                                       "looping:");

  // Loop through adjacent vertices
  for (std::size_t i = 0; i < adjVertices.size() && !spannerPropertyWasViolated; ++i)
  {
    SparseVertex vp = adjVertices[i];  // vp = v' from paper

    BOLT_DEBUG(indent + 2, vQuality_, "Checking v' = " << vp << " ----------------------------");

    // Compute all nodes which qualify as a candidate v" for v and vp
    getAdjVerticesOfV1UnconnectedToV2(v, vp, adjVerticesUnconnected, indent + 4);

    // for each vertex v'' that is adjacent to v (has a valid edge) and does not share an edge with v'
    foreach (SparseVertex vpp, adjVerticesUnconnected)  // vpp = v'' from paper
    {
      BOLT_DEBUG(indent + 4, vQuality_, "Checking v'' = " << vpp);

      InterfaceData &iData = sg_->getInterfaceData(v, vp, vpp, indent + 6);

      // Check if we need to actually add path
      if (spannerTestOriginal(v, vp, vpp, iData, indent + 2))
      // if (spannerTestOuter(v, vp, vpp, iData, indent + 2))
      // if (spannerTestAStar(v, vp, vpp, iData, indent + 2))
      {
        // Actually add the vertices and possibly edges
        if (addQualityPath(v, vp, vpp, iData, indent + 6))
          spannerPropertyWasViolated = true;
      }

    }  // foreach vpp
  }    // foreach vp

  if (!spannerPropertyWasViolated)
  {
    BOLT_DEBUG(indent, vQuality_, "Spanner property was NOT violated, SKIPPING");
  }

  return spannerPropertyWasViolated;
}

void SparseCriteria::visualizeCheckAddPath(SparseVertex v, SparseVertex vp, SparseVertex vpp, InterfaceData &iData,
                                     std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "visualizeCheckAddPath()");
  visual_->viz5DeleteAllMarkers();

  // Show candidate rep
  visual_->viz5State(sg_->getVertexState(v), tools::LARGE, tools::BLUE, 0);

  // Show adjacent state
  visual_->viz5State(sg_->getVertexState(vp), tools::LARGE, tools::PURPLE, 0);
  // visual_->viz5State(sg_->getVertexState(vp), tools::VARIABLE_SIZE, tools::TRANSLUCENT_LIGHT, sparseDelta_);

  // Show edge between them
  visual_->viz5Edge(sg_->getVertexState(vp), sg_->getVertexState(v), tools::eGREEN);

  // Show adjacent state
  visual_->viz5State(sg_->getVertexState(vpp), tools::LARGE, tools::PURPLE, 0);
  // visual_->viz5State(sg_->getVertexState(vpp), tools::VARIABLE_SIZE, tools::TRANSLUCENT_LIGHT, sparseDelta_);

  // Show edge between them
  visual_->viz5Edge(sg_->getVertexState(vpp), sg_->getVertexState(v), tools::eORANGE);
  visual_->viz5Trigger();
  usleep(0.001 * 1000000);

  // Show iData
  if (iData.hasInterface1())
  {
    // std::cout << "hasInterface1 " << std::flush;
    visual_->viz5State(iData.getInterface1Inside(), tools::MEDIUM, tools::ORANGE, 0);
    // std::cout << "1 " << std::flush;
    // std::cout << "state: " << iData.getInterface1Outside() << std::flush;
    visual_->viz5State(iData.getInterface1Outside(), tools::MEDIUM, tools::GREEN, 0);
    // std::cout << " 2 " << std::flush;
    visual_->viz5Edge(iData.getInterface1Inside(), iData.getInterface1Outside(), tools::eRED);
    // std::cout << "3 " << std::endl;

    if (vp < vpp)
      visual_->viz5Edge(sg_->getVertexState(vp), iData.getInterface1Outside(), tools::eRED);
    else
      visual_->viz5Edge(sg_->getVertexState(vpp), iData.getInterface1Outside(), tools::eRED);
  }
  if (iData.hasInterface2())
  {
    visual_->viz5State(iData.getInterface2Inside(), tools::MEDIUM, tools::ORANGE, 0);
    visual_->viz5State(iData.getInterface2Outside(), tools::MEDIUM, tools::GREEN, 0);
    visual_->viz5Edge(iData.getInterface2Inside(), iData.getInterface2Outside(), tools::eRED);

    if (vp < vpp)
      visual_->viz5Edge(sg_->getVertexState(vpp), iData.getInterface2Outside(), tools::eRED);
    else
      visual_->viz5Edge(sg_->getVertexState(vp), iData.getInterface2Outside(), tools::eRED);
  }

  visual_->viz5Trigger();
  usleep(0.001 * 1000000);
}

bool SparseCriteria::addQualityPath(SparseVertex v, SparseVertex vp, SparseVertex vpp, InterfaceData &iData,
                              std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "addQualityPath()");
  indent += 2;

  // Can we connect these two vertices directly?
  const std::size_t threadID = 0;
  if (denseCache_->checkMotionWithCacheVertex(vp, vpp, threadID))
  {
    BOLT_DEBUG(indent, vQuality_, "Adding edge between vp and vpp");

    if (sg_->hasEdge(vp, vpp) || sg_->hasEdge(vpp, vp))
    {
      OMPL_ERROR("Already has an edge!");
      visual_->waitForUserFeedback("has edge");
      exit(-1);
    }

    //SparseEdge e =
    sg_->addEdge(vp, vpp, eQUALITY, indent + 2);

    // if (edgeWeightProperty_[e] > ignoreEdgesSmallerThan_)  // discretization_ + small)
    // {
    //   if (visualizeQualityCriteria_)
    //     visualizeCheckAddPath(v, vp, vpp, iData, indent + 4);

    //   // TEMP:
    //   // std::cout << "discretization_ + small: " << discretization_ + small << std::endl;
    //   // BOLT_DEBUG(0, true, "Spanner property violated, edge added of length " << edgeWeightProperty_[e]);
    //   // visual_->waitForUserFeedback();
    // }

    return true;
  }

  // Super debug
  if (sg_->superDebug_ && si_->checkMotion(sg_->getVertexState(vp), sg_->getVertexState(vpp)))
  {
    OMPL_ERROR("Failed test - edge was in collision in cache, but not from scratch");
    visual_->waitForUserFeedback("error");
  }

  BOLT_YELLOW_DEBUG(indent, visualizeQualityCriteria_, "Unable to connect directly - geometric path must be created "
                                                       "for spanner");

  if (visualizeQualityCriteria_)                           // TEMP
    visualizeCheckAddPath(v, vp, vpp, iData, indent + 4);  // TEMP

  geometric::PathGeometric *path = new geometric::PathGeometric(si_);

  // Populate path
  if (vp < vpp)
  {
    path->append(sg_->getVertexState(vp));
    path->append(iData.getInterface1Outside());
    path->append(iData.getInterface1Inside());
    path->append(sg_->getVertexState(v));
    path->append(iData.getInterface2Inside());
    path->append(iData.getInterface2Outside());
    path->append(sg_->getVertexState(vpp));
  }
  else
  {
    path->append(sg_->getVertexState(vp));
    path->append(iData.getInterface2Outside());
    path->append(iData.getInterface2Inside());
    path->append(sg_->getVertexState(v));
    path->append(iData.getInterface1Inside());
    path->append(iData.getInterface1Outside());
    path->append(sg_->getVertexState(vpp));
  }

  // Create path and simplify
  if (useOriginalSmoother_)
    sg_->smoothQualityPathOriginal(path, indent + 4);
  else
    sg_->smoothQualityPath(path, obstacleClearance_, indent + 4);

  // Insert simplified path into graph
  SparseVertex prior = vp;
  SparseVertex newVertex;
  std::vector<base::State *> &states = path->getStates();

  BOLT_DEBUG(indent + 2, vQuality_, "Shortcuted path now has " << path->getStateCount() << " states");

  if (states.size() < 3)
  {
    BOLT_RED_DEBUG(indent + 2, true, "Somehow path was shrunk to less than three vertices: " << states.size());
    // visual_->waitForUserFeedback("path shrunk to two");
    delete path;
    return false;
  }

  bool addEdgeEnabled = true;                          // if a vertex is skipped, stop adding edges
  for (std::size_t i = 1; i < states.size() - 1; ++i)  // first and last states are vp and vpp, don't sg_->addVertex()
  {
    base::State *state = states[i];

    // Check if this vertex already exists
    if (si_->distance(sg_->getVertexState(v), state) < denseDelta_)  // TODO: is it ok to re-use this denseDelta var?
    {
      BOLT_RED_DEBUG(indent + 2, vQuality_, "Add path state is too similar to v!");

      if (visualizeQualityCriteria_)
      {
        visual_->viz2DeleteAllMarkers();
        visual_->viz2Path(path, 1, tools::RED);
        visual_->viz2Trigger();
        usleep(0.001 * 1000000);

        visual_->waitForUserFeedback("Add path state is too similar to v");
      }

      delete path;
      return false;
    }

    // Check if new vertex has enough clearance
    if (!sufficientClearance(state))
    {
      BOLT_YELLOW_DEBUG(indent + 2, true, "Skipped adding vertex in new path b/c insufficient clearance");
      visual_->waitForUserFeedback("insufficient clearance");
      addEdgeEnabled = false;
      continue;
    }

    // no need to clone state, since we will destroy p; we just copy the pointer
    BOLT_DEBUG(indent + 2, vQuality_, "Adding node from shortcut path for QUALITY");
    newVertex = sg_->addVertex(si_->cloneState(state), QUALITY, indent + 4);

    // Check if there are really close vertices nearby which should be merged
    if (checkRemoveCloseVertices(newVertex, indent + 4))
    {
      // New vertex replaced a nearby vertex, we can continue no further because graph has been re-indexed

      // Remove all edges from all vertices near our new vertex
      sg_->clearEdgesNearVertex(newVertex);

      // TODO should we clearEdgesNearVertex before return true ?
      delete path;
      return true;
    }

    // Remove all edges from all vertices near our new vertex
    sg_->clearEdgesNearVertex(newVertex);

    if (addEdgeEnabled)
    {
      assert(prior != newVertex);
      sg_->addEdge(prior, newVertex, eQUALITY, indent + 2);
      prior = newVertex;
    }
  }

  // clear the states, so memory is not freed twice
  states.clear();

  if (addEdgeEnabled)
  {
    assert(prior != vpp);
    sg_->addEdge(prior, vpp, eQUALITY, indent + 2);
  }

  delete path;

  if (visualizeQualityCriteria_)
    visualizeCheckAddPath(v, vp, vpp, iData, indent + 4);

  return true;
}

bool SparseCriteria::spannerTestOriginal(SparseVertex v, SparseVertex vp, SparseVertex vpp, InterfaceData &iData,
                                   std::size_t indent)
{
  const bool verbose = vQuality_;

  // Computes all nodes which qualify as a candidate x for v, v', and v"
  double midpointPathLength = maxSpannerPath(v, vp, vpp, indent + 6);

  // Check if spanner property violated
  // if (iData.getLastDistance() == 0)  // DTC added zero check
  // {
  //   BOLT_RED_DEBUG(indent + 6, verbose, "iData.getLastDistance() is 0");
  // }
  if (stretchFactor_ * iData.getLastDistance() < midpointPathLength)
  {
    BOLT_YELLOW_DEBUG(indent + 6, verbose, "Spanner property violated");
    BOLT_DEBUG(indent + 8, verbose, "Sparse Graph Midpoint Length  = " << midpointPathLength);
    BOLT_DEBUG(indent + 8, verbose, "Spanner Path Length * Stretch = " << (stretchFactor_ * iData.getLastDistance()));
    BOLT_DEBUG(indent + 10, verbose, "last distance = " << iData.getLastDistance());
    BOLT_DEBUG(indent + 10, verbose, "stretch factor = " << stretchFactor_);
    double rejectStretchFactor = midpointPathLength / iData.getLastDistance();
    BOLT_DEBUG(indent + 10, verbose, "to reject, stretch factor > " << rejectStretchFactor);

    return true;  // spanner property was violated
  }

  BOLT_DEBUG(indent + 6, vQuality_, "Spanner property not violated");
  return false;  // spanner property was NOT violated
}

bool SparseCriteria::spannerTestOuter(SparseVertex v, SparseVertex vp, SparseVertex vpp, InterfaceData &iData,
                                std::size_t indent)
{
  // Computes all nodes which qualify as a candidate x for v, v', and v"
  double midpointPathLength = maxSpannerPath(v, vp, vpp, indent + 6);

  // Must have both interfaces to continue
  if (!iData.hasInterface1() || !iData.hasInterface2())
  {
    return false;
  }

  double newDistance =
      si_->distance(iData.getInterface1Outside(), iData.getInterface2Outside());  // TODO(davetcoleman): cache?

  // Check if spanner property violated
  if (newDistance == 0)  // DTC added zero check
  {
    BOLT_RED_DEBUG(indent + 6, vQuality_, "new distance is 0");
    exit(-1);
  }
  else if (stretchFactor_ * newDistance < midpointPathLength)
  {
    BOLT_YELLOW_DEBUG(indent + 6, vQuality_, "Spanner property violated");
    BOLT_DEBUG(indent + 8, vQuality_, "Sparse Graph Midpoint Length  = " << midpointPathLength);
    BOLT_DEBUG(indent + 8, vQuality_, "Spanner Path Length * Stretch = " << (stretchFactor_ * newDistance));
    BOLT_DEBUG(indent + 10, vQuality_, "new distance = " << newDistance);
    BOLT_DEBUG(indent + 10, vQuality_, "stretch factor = " << stretchFactor_);

    return true;  // spannerPropertyWasViolated
  }
  else
    BOLT_DEBUG(indent + 6, vQuality_, "Spanner property not violated");

  return false;  // spannerPropertyWasViolated = false
}

bool SparseCriteria::spannerTestAStar(SparseVertex v, SparseVertex vp, SparseVertex vpp, InterfaceData &iData,
                                std::size_t indent)
{
  if (iData.hasInterface1() && iData.hasInterface2())
  {
    BOLT_DEBUG(indent + 6, vQuality_, "Temp recalculated distance: " << si_->distance(iData.getInterface1Inside(),
                                                                                      iData.getInterface2Inside()));

    // Experimental calculations
    double pathLength = 0;
    std::vector<SparseVertex> vertexPath;
    if (!sg_->astarSearch(vp, vpp, vertexPath, pathLength, indent + 6))
    {
      BOLT_RED_DEBUG(indent + 6, vQuality_, "No path found");
      visual_->waitForUserFeedback("No path found");
    }
    else
    {
      if (visualizeQualityCriteria_)
      {
        visual_->viz6DeleteAllMarkers();
        assert(vertexPath.size() > 1);
        for (std::size_t i = 1; i < vertexPath.size(); ++i)
        {
          visual_->viz6Edge(sg_->getVertexState(vertexPath[i - 1]), sg_->getVertexState(vertexPath[i]), tools::eGREEN);
        }
      }

      // Add connecting segments:
      double connector1 = si_->distance(sg_->getVertexState(vp), iData.getOutsideInterfaceOfV1(vp, vpp));
      // TODO(davetcoleman): may want to include the dist from inside to outside of interface
      BOLT_DEBUG(indent + 6, vQuality_, "connector1 " << connector1);
      if (visualizeQualityCriteria_)
      {
        visual_->viz6Edge(sg_->getVertexState(vp), iData.getOutsideInterfaceOfV1(vp, vpp), tools::eORANGE);
      }

      double connector2 = si_->distance(sg_->getVertexState(vpp), iData.getOutsideInterfaceOfV2(vp, vpp));
      // TODO(davetcoleman): may want to include the dist from inside to outside of interface
      BOLT_DEBUG(indent + 6, vQuality_, "connector2 " << connector2);
      if (visualizeQualityCriteria_)
      {
        visual_->viz6Edge(sg_->getVertexState(vpp), iData.getOutsideInterfaceOfV2(vp, vpp), tools::eYELLOW);
      }

      pathLength += connector1 + connector2;
      BOLT_DEBUG(indent + 6, vQuality_, "Full Path Length: " << pathLength);

      visual_->viz6Trigger();
    }

    if (iData.getLastDistance() == 0)
    {
      BOLT_YELLOW_DEBUG(indent + 6, vQuality_, "Last distance is 0");
    }

    // Theoretical max
    // double theoreticalMaxLength = iData.getLastDistance() + 2 * sparseDelta_;
    double theoreticalMaxLength = iData.getLastDistance() + sparseDelta_;
    BOLT_DEBUG(indent + 6, vQuality_, "Max allowable length: " << theoreticalMaxLength);
    theoreticalMaxLength *= stretchFactor_;
    BOLT_DEBUG(indent + 6, vQuality_, "Max allowable length with stretch: " << theoreticalMaxLength);

    if (pathLength < theoreticalMaxLength)
    {
      BOLT_DEBUG(indent + 6, vQuality_, "Astar says we do not need to add an edge");
    }
    else
    {
      BOLT_RED_DEBUG(indent + 6, vQuality_, "Astar says we need to add an edge");

      return true;  // spannerPropertyWasViolated = true
    }
  }

  return false;  // spannerPropertyWasViolated = false
}

SparseVertex SparseCriteria::findGraphRepresentative(base::State *state, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "findGraphRepresentative()");

  std::vector<SparseVertex> graphNeighbors;
  const std::size_t threadID = 0;

  // Search
  sg_->queryStates_[threadID] = state;
  sg_->nn_->nearestR(sg_->queryVertices_[threadID], sparseDelta_, graphNeighbors);
  sg_->queryStates_[threadID] = nullptr;

  BOLT_DEBUG(indent + 2, vQuality_, "Found " << graphNeighbors.size() << " nearest neighbors (graph rep) within "
                                                                         "SparseDelta " << sparseDelta_);

  SparseVertex result = boost::graph_traits<SparseAdjList>::null_vertex();

  for (std::size_t i = 0; i < graphNeighbors.size(); ++i)
  {
    BOLT_DEBUG(indent + 2, vQuality_, "Checking motion of graph representative candidate " << i);
    if (si_->checkMotion(state, sg_->getVertexState(graphNeighbors[i])))
    {
      BOLT_DEBUG(indent + 4, vQuality_, "graph representative valid ");
      result = graphNeighbors[i];
      break;
    }
    else
      BOLT_DEBUG(indent + 4, vQuality_, "graph representative NOT valid, checking next ");
  }
  return result;
}

void SparseCriteria::findCloseRepresentatives(base::State *workState, const StateID candidateStateID,
                                        const SparseVertex candidateRep,
                                        std::map<SparseVertex, base::State *> &closeRepresentatives, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "findCloseRepresentatives()");
  BOLT_DEBUG(indent + 2, vQuality_, "nearSamplePoints: " << nearSamplePoints_ << " denseDelta: " << denseDelta_);
  const bool visualizeSampler = false;
  base::State *sampledState = workState;  // rename variable just to clarify what it represents temporarily

  assert(closeRepresentatives.empty());

  // Search the space around new potential state candidateState
  for (std::size_t i = 0; i < nearSamplePoints_; ++i)
  {
    BOLT_DEBUG(indent + 2, vQuality_, "Get supporting representative #" << i);

    bool foundValidSample = false;
    static const std::size_t MAX_SAMPLE_ATTEMPT = 1000;
    for (std::size_t attempt = 0; attempt < MAX_SAMPLE_ATTEMPT; ++attempt)
    {
      BOLT_DEBUG(indent + 4, vQuality_, "Sample attempt " << attempt);

      regularSampler_->sampleNear(sampledState, denseCache_->getState(candidateStateID), denseDelta_);
      // si_->getStateSpace()->setLevel(sampledState, 0);  // TODO no hardcode

      if (!si_->isValid(sampledState))
      {
        BOLT_DEBUG(indent + 6, vQuality_, "notValid ");

        if (visualizeQualityCriteria_ && visualizeSampler)
          visual_->viz3State(sampledState, tools::SMALL, tools::RED, 0);

        continue;
      }
      if (si_->distance(denseCache_->getState(candidateStateID), sampledState) > denseDelta_)
      {
        BOLT_DEBUG(indent + 6, vQuality_, "Distance too far "
                                              << si_->distance(denseCache_->getState(candidateStateID), sampledState)
                                              << " needs to be less than " << denseDelta_);

        if (visualizeQualityCriteria_ && visualizeSampler)
          visual_->viz3State(sampledState, tools::SMALL, tools::RED, 0);
        continue;
      }
      if (!si_->checkMotion(denseCache_->getState(candidateStateID), sampledState))
      {
        BOLT_DEBUG(indent + 6, vQuality_, "Motion invalid ");

        if (visualizeQualityCriteria_ && visualizeSampler)
          visual_->viz3State(sampledState, tools::SMALL, tools::RED, 0);
        continue;
      }

      if (visualizeQualityCriteria_ && visualizeSampler)
        visual_->viz3State(sampledState, tools::SMALL, tools::GREEN, 0);

      foundValidSample = true;
      break;
    }  // for each attempt

    if (visualizeQualityCriteria_ && visualizeSampler)
    {
      visual_->viz3Trigger();
      usleep(0.001 * 1000000);
    }

    if (!foundValidSample)
    {
      BOLT_DEBUG(indent + 4, vQuality_, "Unable to find valid sample after " << MAX_SAMPLE_ATTEMPT << " attempts"
                                                                                                      " ");
    }
    else
      BOLT_DEBUG(indent + 4, vQuality_, "Found valid nearby sample");

    // Compute which sparse vertex represents this new candidate vertex
    SparseVertex sampledStateRep = findGraphRepresentative(sampledState, indent + 6);

    // Check if sample is not visible to any other node (it should be visible in all likelihood)
    if (sampledStateRep == boost::graph_traits<SparseAdjList>::null_vertex())
    {
      BOLT_DEBUG(indent + 4, vQuality_, "Sampled state has no representative (is null) ");

      // It can't be seen by anybody, so we should take this opportunity to add him
      // But first check for proper clearance
      if (sufficientClearance(sampledState))
      {
        BOLT_DEBUG(indent + 4, vQuality_, "Adding node for COVERAGE");
        sg_->addVertex(si_->cloneState(sampledState), COVERAGE, indent + 4);
      }

      BOLT_DEBUG(indent + 4, vQuality_, "STOP EFFORTS TO ADD A DENSE PATH");

      // We should also stop our efforts to add a dense path
      for (std::map<SparseVertex, base::State *>::iterator it = closeRepresentatives.begin();
           it != closeRepresentatives.end(); ++it)
        si_->freeState(it->second);
      closeRepresentatives.clear();
      break;
    }

    BOLT_DEBUG(indent + 4, vQuality_, "Sampled state has representative (is not null)");

    // If its representative is different than candidateState
    if (sampledStateRep != candidateRep)
    {
      BOLT_DEBUG(indent + 4, vQuality_, "candidateRep != sampledStateRep ");

      // And we haven't already tracked this representative
      if (closeRepresentatives.find(sampledStateRep) == closeRepresentatives.end())
      {
        BOLT_DEBUG(indent + 4, vQuality_, "Track the representative");

        // Track the representative
        closeRepresentatives[sampledStateRep] = si_->cloneState(sampledState);
      }
      else
      {
        BOLT_DEBUG(indent + 4, vQuality_, "Already tracking the representative");
      }
    }
    else
    {
      BOLT_DEBUG(indent + 4, vQuality_, "candidateRep == sampledStateRep, do not keep this sample ");
    }
  }  // for each supporting representative
}

bool SparseCriteria::updatePairPoints(SparseVertex candidateRep, const base::State *candidateState,
                                SparseVertex nearSampledRep, const base::State *nearSampledState, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "updatePairPoints()");
  bool updated = false;  // track whether a change was made to any vertices' representatives

  // First of all, we need to compute all candidate r'
  std::vector<SparseVertex> adjVerticesUnconnected;
  getAdjVerticesOfV1UnconnectedToV2(candidateRep, nearSampledRep, adjVerticesUnconnected, indent + 2);

  // for each pair Pv(r,r')
  foreach (SparseVertex adjVertexUnconnected, adjVerticesUnconnected)
  {
    // Try updating the pair info
    if (distanceCheck(candidateRep, candidateState, nearSampledRep, nearSampledState, adjVertexUnconnected, indent + 2))
      updated = true;
  }

  return updated;
}

void SparseCriteria::getAdjVerticesOfV1UnconnectedToV2(SparseVertex v1, SparseVertex v2,
                                                 std::vector<SparseVertex> &adjVerticesUnconnected, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "getAdjVerticesOfV1UnconnectedToV2()");

  adjVerticesUnconnected.clear();
  foreach (SparseVertex adjVertex, boost::adjacent_vertices(v1, sg_->getGraph()))
    if (adjVertex != v2)
      if (!sg_->hasEdge(adjVertex, v2))
        adjVerticesUnconnected.push_back(adjVertex);

  BOLT_DEBUG(indent + 2, vQuality_, "adjVerticesUnconnected size: " << adjVerticesUnconnected.size());
}

double SparseCriteria::maxSpannerPath(SparseVertex v, SparseVertex vp, SparseVertex vpp, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "maxSpannerPath()");
  indent += 2;

  // Candidate x vertices as described in paper in Max_Spanner_Path
  std::vector<SparseVertex> qualifiedVertices;

  // Get nearby vertices 'x' that could also be used to find the path to v''
  if (true)
  {
    foreach (SparseVertex x, boost::adjacent_vertices(vpp, sg_->getGraph()))
    {
      if (sg_->hasEdge(x, v) && !sg_->hasEdge(x, vp))
      {
        InterfaceData &iData = sg_->getInterfaceData(v, vpp, x, indent + 2);

        // Check if we previously had found a pair of points that support this interface
        if ((vpp < x && iData.getInterface1Inside()) || (x < vpp && iData.getInterface2Inside()))
        {
          BOLT_RED_DEBUG(indent, vQuality_, "Found an additional qualified vertex " << x);
          // This is a possible alternative path to v''
          qualifiedVertices.push_back(x);

          if (visualizeQualityCriteria_)
          {
            visual_->viz5State(sg_->getVertexState(x), tools::LARGE, tools::BLACK, 0);

            // Temp?
            // visual_->viz6DeleteAllMarkers();
            // visual_->viz6State(sg_->getVertexState(x), tools::LARGE, tools::BLACK, 0);
            // visual_->viz6Trigger();
          }
        }
      }
    }
  }
  else
    BOLT_YELLOW_DEBUG(indent, vQuality_, "Disabled nearby vertices in maxSpannerPath");

  // vpp is always qualified because of its previous checks
  qualifiedVertices.push_back(vpp);
  BOLT_DEBUG(indent, vQuality_, "Total qualified vertices found: " << qualifiedVertices.size());

  // Find the maximum spanner distance
  BOLT_DEBUG(indent, vQuality_, "Finding the maximum spanner distance between v' and v''");
  double maxDist = 0.0;
  foreach (SparseVertex qualifiedVertex, qualifiedVertices)
  {
    if (visualizeQualityCriteria_)
      visual_->viz5State(sg_->getVertexState(qualifiedVertex), tools::SMALL, tools::PINK, 0);

    // Divide by 2 because of the midpoint path 'M'
    double tempDist = (si_->distance(sg_->getVertexState(vp), sg_->getVertexState(v)) +
                       si_->distance(sg_->getVertexState(v), sg_->getVertexState(qualifiedVertex))) /
                      2.0;
    BOLT_DEBUG(indent + 2, vQuality_, "Checking vertex: " << qualifiedVertex << " distance: " << tempDist);

    if (tempDist > maxDist)
    {
      BOLT_DEBUG(indent + 4, vQuality_, "Is larger than previous");
      maxDist = tempDist;
    }
  }
  BOLT_DEBUG(indent, vQuality_, "Max distance: " << maxDist);

  return maxDist;
}

bool SparseCriteria::distanceCheck(SparseVertex v, const base::State *q, SparseVertex vp, const base::State *qp,
                             SparseVertex vpp, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "distanceCheck()");
  indent += 2;

  bool updated = false;  // track whether a change was made to any vertices' representatives

  // Get the info for the current representative-neighbors pair
  InterfaceData &iData = sg_->getInterfaceData(v, vp, vpp, indent + 4);

  if (vp < vpp)  // FIRST points represent r (the interface discovered through sampling)
  {
    if (!iData.hasInterface1())  // No previous interface has been found here, just save it
    {
      BOLT_DEBUG(indent, vQuality_, "setInterface1");
      iData.setInterface1(q, qp, si_);
      updated = true;
    }
    else if (!iData.hasInterface2())  // The other interface doesn't exist, so we can't compare.
    {
      // Should probably keep the one that is further away from rep?  Not known what to do in this case.
      // TODO: is this not part of the algorithm?
      BOLT_YELLOW_DEBUG(indent, vQuality_, "TODO no interface 2");
    }
    else  // We know both of these points exist, so we can check some distances
    {
      assert(iData.getLastDistance() < std::numeric_limits<double>::infinity());
      if (si_->distance(q, iData.getInterface2Inside()) < iData.getLastDistance())
      // si_->distance(iData.getInterface1Inside(), iData.getInterface2Inside()))
      {  // Distance with the new point is good, so set it.
        BOLT_GREEN_DEBUG(indent, vQuality_, "setInterface1 UPDATED");
        iData.setInterface1(q, qp, si_);
        updated = true;
      }
      else
      {
        BOLT_DEBUG(indent, vQuality_, "Distance was not better, not updating bookkeeping");
      }
    }
  }
  else  // SECOND points represent r (the interfaec discovered through sampling)
  {
    if (!iData.hasInterface2())  // No previous interface has been found here, just save it
    {
      BOLT_DEBUG(indent, vQuality_, "setInterface2");
      iData.setInterface2(q, qp, si_);
      updated = true;
    }
    else if (!iData.hasInterface1())  // The other interface doesn't exist, so we can't compare.
    {
      // Should we be doing something cool here?
      BOLT_YELLOW_DEBUG(indent, vQuality_, "TODO no interface 1");
    }
    else  // We know both of these points exist, so we can check some distances
    {
      assert(iData.getLastDistance() < std::numeric_limits<double>::infinity());
      if (si_->distance(q, iData.getInterface1Inside()) < iData.getLastDistance())
      // si_->distance(iData.getInterface2Inside(), iData.getInterface1Inside()))
      {  // Distance with the new point is good, so set it
        BOLT_GREEN_DEBUG(indent, vQuality_, "setInterface2 UPDATED");
        iData.setInterface2(q, qp, si_);
        updated = true;
      }
      else
      {
        BOLT_DEBUG(indent, vQuality_, "Distance was not better, not updating bookkeeping");
      }
    }
  }

  // Lastly, save what we have discovered
  if (updated)
  {
    // TODO(davetcoleman): do we really need to copy this back in or is it already passed by reference?
    sg_->interfaceDataProperty_[v][sg_->interfaceDataIndex(vp, vpp)] = iData;
  }

  return updated;
}

void SparseCriteria::findGraphNeighbors(StateID candidateStateID, std::vector<SparseVertex> &graphNeighborhood,
                                  std::vector<SparseVertex> &visibleNeighborhood, std::size_t threadID,
                                  std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vCriteria_, "findGraphNeighbors()");
  const bool verbose = false;

  // Search
  sg_->queryStates_[threadID] = denseCache_->getStateNonConst(candidateStateID);
  sg_->nn_->nearestR(sg_->queryVertices_[threadID], sparseDelta_, graphNeighborhood);
  sg_->queryStates_[threadID] = nullptr;

  // Now that we got the neighbors from the NN, we must remove any we can't see
  for (std::size_t i = 0; i < graphNeighborhood.size(); ++i)
  {
    SparseVertex v2 = graphNeighborhood[i];

    // Don't collision check if they are the same state
    if (candidateStateID != sg_->getStateID(v2))
    {
      if (!denseCache_->checkMotionWithCache(candidateStateID, sg_->getStateID(v2), threadID))
      {
        continue;
      }
    }
    else if (verbose)
      std::cout << " ---- Skipping collision checking because same vertex " << std::endl;

    // The two are visible to each other!
    visibleNeighborhood.push_back(graphNeighborhood[i]);
  }

  BOLT_DEBUG(indent + 2, vCriteria_, "Graph neighborhood: " << graphNeighborhood.size() << " | Visible neighborhood: "
                                                            << visibleNeighborhood.size());
}

bool SparseCriteria::checkRemoveCloseVertices(SparseVertex v1, std::size_t indent)
{
  // This feature can be optionally disabled
  if (!useCheckRemoveCloseVertices_)
    return false;

  BOLT_CYAN_DEBUG(indent, vRemoveClose_, "checkRemoveCloseVertices() v1 = " << v1);
  indent += 2;

  // Get neighbors
  std::vector<SparseVertex> graphNeighbors;
  const std::size_t numNeighbors = 2;  // the first neighbor is (usually?) the vertex itself
  sg_->nn_->nearestK(v1, numNeighbors, graphNeighbors);

  if (vRemoveClose_)
  {
    std::stringstream ss;
    std::copy(graphNeighbors.begin(), graphNeighbors.end(), std::ostream_iterator<SparseVertex>(ss, ","));
    BOLT_DEBUG(indent, vRemoveClose_, "Neighbors of " << v1 << " are: " << ss.str());
  }

  // Error check no neighbors
  if (graphNeighbors.size() <= 1)
  {
    BOLT_RED_DEBUG(indent, vRemoveClose_, "checkRemoveCloseVertices: no neighbors found for sparse vertex " << v1);
    return false;
  }

  SparseVertex v2 = graphNeighbors[1];
  double sparseDeltaFractionCheck = 0.5;  // 0.25;  // TODO: determine better value for this

  // Error check: Do not remove itself
  if (v1 == v2)
  {
    BOLT_RED_DEBUG(indent, vRemoveClose_, "checkRemoveCloseVertices: error occured, cannot remove self: " << v1);
    exit(-1);
  }

  // Check that nearest neighbor is not an interface node - these should not be moved
  if (sg_->vertexTypeProperty_[v2] == QUALITY)
  {
    if (visualizeRemoveCloseVertices_)
    {
      visualizeRemoveCloseVertices(v1, v2);
      visual_->waitForUserFeedback("Skipping this vertex because is QUALITY");
    }
    return false;
  }

  // Check if nearest neighbor is within distance threshold
  if (sg_->distanceFunction(v1, v2) > sparseDelta_ * sparseDeltaFractionCheck)
  {
    // BOLT_DEBUG(indent, vRemoveClose_, "Distance " << sg_->distanceFunction(v1, v2) << " is greater than max "
    //<< sparseDelta_ * sparseDeltaFractionCheck);
    return false;
  }

  // Check if nearest neighbor is collision free
  if (!si_->checkMotion(sg_->getVertexState(v1), sg_->getVertexState(v2)))
  {
    BOLT_RED_DEBUG(indent, vRemoveClose_, "checkRemoveCloseVertices: not collision free v1=" << v1 << ", v2=" << v2);
    return false;
  }

  BOLT_DEBUG(indent, vRemoveClose_, "Found visible nearby node, testing if able to replace " << v2 << " with " << v1);

  // Nearest neighbor is good candidate, next check if all of its connected neighbors can be connected to new vertex
  foreach (SparseEdge edge, boost::out_edges(v2, sg_->getGraph()))
  {
    SparseVertex v3 = boost::target(edge, sg_->getGraph());
    BOLT_DEBUG(indent + 2, vRemoveClose_, "checking edge v1= " << v1 << " to v3=" << v3);

    // Check if distance is within sparseDelta
    if (si_->distance(sg_->getVertexState(v1), sg_->getVertexState(v3)) > sparseDelta_)
    {
      BOLT_DEBUG(indent + 2, vRemoveClose_, "checkRemoveCloseVertices: distance too far from previous neighbor " << v3);
      return false;
    }

    // Check if collision free path to connected vertex
    if (!si_->checkMotion(sg_->getVertexState(v1), sg_->getVertexState(v3)))
    {
      BOLT_RED_DEBUG(indent + 2, vRemoveClose_,
                     "checkRemoveCloseVertices: not collision free path from new vertex to potential neighbor " << v3);
      return false;
    }
  }

  BOLT_DEBUG(indent, vRemoveClose_, "Found qualified node to replace with nearby");

  if (visualizeRemoveCloseVertices_)
  {
    visualizeRemoveCloseVertices(v1, v2);
    visual_->waitForUserFeedback("found qualified node to replace with nearby");
  }

  // Remove all interface data for old state
  sg_->clearInterfaceData(sg_->getVertexStateNonConst(v2));

  // Connect new vertex to old vertex's connected neighbors
  foreach (SparseEdge edge, boost::out_edges(v2, sg_->getGraph()))
  {
    SparseVertex v3 = boost::target(edge, sg_->getGraph());
    BOLT_DEBUG(indent + 2, vRemoveClose_, "Connecting v1= " << v1 << " to v3=" << v3);
    sg_->addEdge(v1, v3, eINTERFACE, indent + 4);
  }

  // Delete old vertex
  sg_->removeVertex(v2);
  BOLT_DEBUG(indent, vRemoveClose_, "REMOVING VERTEX " << v2 << " which was replaced with " << v1 << " with state ");

  // Statistics
  numVerticesMoved_++;

  // Only display database if enabled
  if (sg_->visualizeSparsGraph_ && sg_->visualizeSparsGraphSpeed_ > std::numeric_limits<double>::epsilon())
    sg_->displayDatabase(true, indent + 4);

  if (visualizeRemoveCloseVertices_)
    visual_->waitForUserFeedback("finished moving vertex");

  if (visualizeRemoveCloseVertices_)
  {
    visual_->viz6DeleteAllMarkers();
    visual_->viz6Trigger();
    usleep(0.001 * 1000000);
  }

  return true;
}

void SparseCriteria::visualizeRemoveCloseVertices(SparseVertex v1, SparseVertex v2)
{
  visual_->viz6DeleteAllMarkers();
  visual_->viz6State(sg_->getVertexState(v1), tools::LARGE, tools::GREEN, 0);
  visual_->viz6State(sg_->getVertexState(v2), tools::LARGE, tools::RED, 0);  // RED = to be removed
  visual_->viz6Trigger();
  usleep(0.001 * 1000000);
}

void SparseCriteria::visualizeInterfaces(SparseVertex v, std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "visualizeInterfaces()");

  InterfaceHash &iData = sg_->interfaceDataProperty_[v];

  visual_->viz6DeleteAllMarkers();
  visual_->viz6State(sg_->getVertexState(v), tools::LARGE, tools::RED, 0);

  for (auto it = iData.begin(); it != iData.end(); ++it)
  {
    const VertexPair &pair = it->first;
    InterfaceData &iData = it->second;

    SparseVertex v1 = pair.first;
    SparseVertex v2 = pair.second;

    visual_->viz6State(sg_->getVertexState(v1), tools::LARGE, tools::PURPLE, 0);
    visual_->viz6State(sg_->getVertexState(v2), tools::LARGE, tools::PURPLE, 0);
    // visual_->viz6Edge(sg_->getVertexState(v1), sg_->getVertexState(v2), tools::eGREEN);

    if (iData.hasInterface1())
    {
      visual_->viz6State(iData.getInterface1Inside(), tools::MEDIUM, tools::ORANGE, 0);
      visual_->viz6State(iData.getInterface1Outside(), tools::MEDIUM, tools::GREEN, 0);
      visual_->viz6Edge(iData.getInterface1Inside(), iData.getInterface1Outside(), tools::eYELLOW);
    }
    else
    {
      visual_->viz6Edge(sg_->getVertexState(v1), sg_->getVertexState(v2), tools::eRED);
    }

    if (iData.hasInterface2())
    {
      visual_->viz6State(iData.getInterface2Inside(), tools::MEDIUM, tools::ORANGE, 0);
      visual_->viz6State(iData.getInterface2Outside(), tools::MEDIUM, tools::GREEN, 0);
      visual_->viz6Edge(iData.getInterface2Inside(), iData.getInterface2Outside(), tools::eYELLOW);
    }
    else
    {
      visual_->viz6Edge(sg_->getVertexState(v1), sg_->getVertexState(v2), tools::eRED);
    }
  }

  visual_->viz6Trigger();
  usleep(0.01 * 1000000);
}

void SparseCriteria::visualizeAllInterfaces(std::size_t indent)
{
  BOLT_BLUE_DEBUG(indent, vQuality_, "visualizeAllInterfaces()");

  visual_->viz6DeleteAllMarkers();

  foreach (SparseVertex v, boost::vertices(sg_->g_))
  {
    // typedef std::unordered_map<VertexPair, InterfaceData> InterfaceHash;
    InterfaceHash &hash = sg_->interfaceDataProperty_[v];

    for (auto it = hash.begin(); it != hash.end(); ++it)
    {
      InterfaceData &iData = it->second;

      if (iData.hasInterface1())
      {
        visual_->viz6State(iData.getInterface1Inside(), tools::MEDIUM, tools::ORANGE, 0);
        visual_->viz6State(iData.getInterface1Outside(), tools::MEDIUM, tools::GREEN, 0);
      }

      if (iData.hasInterface2())
      {
        visual_->viz6State(iData.getInterface2Inside(), tools::MEDIUM, tools::ORANGE, 0);
        visual_->viz6State(iData.getInterface2Outside(), tools::MEDIUM, tools::GREEN, 0);
      }
    }
  }
  visual_->viz6Trigger();
  usleep(0.01 * 1000000);
}

std::pair<std::size_t, std::size_t> SparseCriteria::getInterfaceStateStorageSize()
{
  std::size_t numStates = 0;
  std::size_t numMissingInterfaces = 0;

  foreach (SparseVertex v, boost::vertices(sg_->g_))
  {
    InterfaceHash &hash = sg_->interfaceDataProperty_[v];

    for (auto it = hash.begin(); it != hash.end(); ++it)
    {
      InterfaceData &iData = it->second;

      if (iData.hasInterface1())
        numStates += 2;
      else
        numMissingInterfaces++;

      if (iData.hasInterface2())
        numStates += 2;
      else
        numMissingInterfaces++;

      // Debug
      if (false)
        if (!iData.hasInterface1() && !iData.hasInterface2())
          std::cout << "has neither interfaces! " << std::endl;
    }
  }
  return std::pair<std::size_t, std::size_t>(numStates, numMissingInterfaces);
}

bool SparseCriteria::sufficientClearance(base::State *state)
{
  // Check if new vertex has enough clearance
  double dist = si_->getStateValidityChecker()->clearance(state);
  return dist >= obstacleClearance_;
}

}  // namespace bolt
}  // namespace tools
}  // namespace ompl
