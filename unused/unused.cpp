/*
// Determine every dense node's representative on the sparse graph
findSparseRepresentatives();

// Check 4th criteria - are we within our stated asymptotic bounds?
std::size_t coutIndent = 0;
OMPL_INFORM("Checking remaining vertices for 4th critera test");
for (std::size_t i = 0; i < vertexInsertionOrder.size(); ++i)
{
if (!checkAsymptoticOptimal(vertexInsertionOrder[i].v_, coutIndent + 4))
{
std::cout << "Vertex " << vertexInsertionOrder[i].v_ << " failed asymptotic optimal test " << std::endl;
}
}
*/

bool checkAsymptoticOptimal(const TaskVertex &denseV, std::size_t coutIndent);
bool checkAddPath(TaskVertex q, const std::vector<TaskVertex> &neigh, std::size_t coutIndent);
void computeVPP(SparseVertex v, SparseVertex vp, std::vector<SparseVertex> &VPPs);
void computeX(SparseVertex v, SparseVertex vp, SparseVertex vpp, std::vector<SparseVertex> &Xs);
bool addPathToSpanner(const DensePath &densePath, SparseVertex vp, SparseVertex vpp);
void connectSparsePoints(SparseVertex v, SparseVertex vp);

/** \brief Calculate representative nodes for each dense verte */
bool findSparseRepresentatives();

bool SparseGraph::findSparseRepresentatives()
{
  bool verbose = false;

  OMPL_INFORM("Calculating representative nodes for each dense verte");
  foreach (TaskVertex denseV, boost::vertices(denseDB_->g_))
  {
    std::vector<SparseVertex> graphNeighborhood;
    base::State *state = getDenseState(denseV);

    // Skip the query vertex 0
    if (denseV == denseDB_->queryVertex_)
      continue;
    assert(denseV);

    if (verbose)
      std::cout << "Searching for denseV: " << denseV << std::endl;

    // Search
    getSparseState(queryVertex_) = state;
    nn_->nearestR(queryVertex_, sparseDelta_, graphNeighborhood);
    getSparseState(queryVertex_) = nullptr;

    // Find the closest sparse node that has a local free path
    bool foundRepresentative = false;
    for (std::size_t i = 0; i < graphNeighborhood.size(); ++i)
    {
      if (verbose)
        std::cout << "  Checking motion for neighbor " << i << std::endl;
      if (si_->checkMotion(state, getSparseState(graphNeighborhood[i])))
      {
        if (verbose)
          std::cout << "   Found valid sparse vertex that is near: " << graphNeighborhood[i] << std::endl;

        // Assign the dense vertex its representative sparse node
        denseDB_->representativesProperty_[denseV] = graphNeighborhood[i];
        foundRepresentative = true;
        break;
      }
    }

    // Error check
    if (!foundRepresentative)
    {
      OMPL_WARN("Unable to find sparse representative for dense vertex %u", denseV);
      exit(-1);
    }

    // Visualize
    if (visualizeDenseRepresentatives_)
    {
      // Ensure that states are not the same
      SparseVertex &sparseV = denseDB_->representativesProperty_[denseV];
      if (denseV == denseVertexProperty_[sparseV])
      {
        if (verbose)
          OMPL_WARN("Not visualizing because the dense vertex's representative sparse vertex are the same");
      }
      else
      {
        double visualColor = 100;
        visual_->viz2Edge(state, getSparseState(denseDB_->representativesProperty_[denseV]), visualColor);
      }
    }
  }
  visual_->viz2Trigger();

  return true;
}

bool SparseGraph::checkAsymptoticOptimal(const TaskVertex &denseV, std::size_t coutIndent)
{
  if (fourthCheckVerbose_)
    std::cout << std::string(coutIndent, ' ') << "checkAsymptoticOptimal()" << std::endl;

  // Storage for the interface neighborhood, populated by getInterfaceNeighborhood()
  std::vector<TaskVertex> interfaceNeighborhood;

  // Check to see if Vertex is on an interface
  getInterfaceNeighborhood(denseV, interfaceNeighborhood, coutIndent + 4);
  if (interfaceNeighborhood.size() > 0)
  {
    if (fourthCheckVerbose_)
      std::cout << std::string(coutIndent + 2, ' ') << "Candidate vertex supports an interface" << std::endl;

    // Check for addition for spanner prop
    if (checkAddPath(denseV, interfaceNeighborhood, coutIndent + 4))
      return true;
  }
  else
  {
    if (fourthCheckVerbose_)
      std::cout << std::string(coutIndent + 2, ' ') << "Candidate vertex does NOT support an interface (no "
                                                       "neighbors)" << std::endl;
  }

  // All of the tests have failed.  Report failure for the sample
  return false;
}

bool SparseGraph::checkAddPath(TaskVertex q, const std::vector<TaskVertex> &neigh, std::size_t coutIndent)
{
  if (fourthCheckVerbose_)
    std::cout << std::string(coutIndent, ' ') << "checkAddPath() TaskVertex: " << q << std::endl;

  bool spannerPropertyViolated = false;

  // Get q's representative => v
  SparseVertex v = denseDB_->representativesProperty_[q];

  // Extract the representatives of neigh => n_rep
  std::set<SparseVertex> neighborReps;
  foreach (TaskVertex qp, neigh)
    neighborReps.insert(denseDB_->representativesProperty_[qp]);

  // Feedback
  if (neighborReps.empty())
    if (fourthCheckVerbose_)
      std::cout << std::string(coutIndent + 2, ' ') << "neighborReps is empty" << std::endl;

  std::vector<SparseVertex> Xs;
  // for each v' in neighborReps
  for (std::set<SparseVertex>::iterator it = neighborReps.begin(); it != neighborReps.end() && !spannerPropertyViolated;
       ++it)
  {
    if (fourthCheckVerbose_)
      std::cout << std::string(coutIndent + 2, ' ') << "for neighborRep " << *it << std::endl;

    SparseVertex vp = *it;
    // Identify appropriate v" candidates => vpps
    std::vector<SparseVertex> VPPs;
    computeVPP(v, vp, VPPs);

    foreach (SparseVertex vpp, VPPs)
    {
      if (fourthCheckVerbose_)
        std::cout << std::string(coutIndent + 4, ' ') << "for VPPs " << vpp << std::endl;

      double s_max = 0;
      // Find the X nodes to test
      computeX(v, vp, vpp, Xs);

      // For each x in xs
      foreach (SparseVertex x, Xs)
      {
        if (fourthCheckVerbose_)
          std::cout << std::string(coutIndent + 6, ' ') << "for Xs " << x << std::endl;

        // Compute/Retain MAXimum distance path thorugh S
        double dist = (si_->distance(getSparseStateConst(x), getSparseStateConst(v)) +
                       si_->distance(getSparseStateConst(v), getSparseStateConst(vp))) /
                      2.0;
        if (dist > s_max)
          s_max = dist;
      }

      DensePath bestDPath;
      TaskVertex best_qpp = boost::graph_traits<DenseGraph>::null_vertex();
      double d_min = std::numeric_limits<double>::infinity();  // Insanely big number
      // For each vpp in vpps
      for (std::size_t j = 0; j < VPPs.size() && !spannerPropertyViolated; ++j)
      {
        if (fourthCheckVerbose_)
          std::cout << std::string(coutIndent + 6, ' ') << "for VPPs " << j << std::endl;

        SparseVertex vpp = VPPs[j];
        // For each q", which are stored interface nodes on v for i(vpp,v)
        foreach (TaskVertex qpp, interfaceListsProperty_[v].interfaceHash[vpp])
        {
          if (fourthCheckVerbose_)
            std::cout << std::string(coutIndent + 8, ' ') << "for interfaceHsh " << qpp << std::endl;

          // check that representatives are consistent
          assert(denseDB_->representativesProperty_[qpp] == v);

          // If they happen to be the one and same node
          if (q == qpp)
          {
            bestDPath.push_front(getDenseState(q));
            best_qpp = qpp;
            d_min = 0;
          }
          else
          {
            // Compute/Retain MINimum distance path on D through q, q"
            DensePath dPath;
            denseDB_->computeDensePath(q, qpp, dPath);
            if (dPath.size() > 0)
            {
              // compute path length
              double length = 0.0;
              DensePath::const_iterator jt = dPath.begin();
              for (DensePath::const_iterator it = jt + 1; it != dPath.end(); ++it)
              {
                length += si_->distance(*jt, *it);
                jt = it;
              }

              if (length < d_min)
              {
                d_min = length;
                bestDPath.swap(dPath);
                best_qpp = qpp;
              }
            }
          }
        }

        // If the spanner property is violated for these paths
        if (s_max > stretchFactor_ * d_min)
        {
          // Need to augment this path with the appropriate neighbor information
          TaskVertex na = getInterfaceNeighbor(q, vp);
          TaskVertex nb = getInterfaceNeighbor(best_qpp, vpp);

          bestDPath.push_front(getDenseState(na));
          bestDPath.push_back(getDenseState(nb));

          // check consistency of representatives
          assert(denseDB_->representativesProperty_[na] == vp && denseDB_->representativesProperty_[nb] == vpp);

          // Add the dense path to the spanner
          addPathToSpanner(bestDPath, vpp, vp);

          // Report success
          spannerPropertyViolated = true;
        }
      }
    }
  }
  return spannerPropertyViolated;
}

void SparseGraph::computeVPP(SparseVertex v, SparseVertex vp, std::vector<SparseVertex> &VPPs)
{
  foreach (SparseVertex cvpp, boost::adjacent_vertices(v, g_))
    if (cvpp != vp)
      if (!boost::edge(cvpp, vp, g_).second)
        VPPs.push_back(cvpp);
}

void SparseGraph::computeX(SparseVertex v, SparseVertex vp, SparseVertex vpp, std::vector<SparseVertex> &Xs)
{
  Xs.clear();
  foreach (SparseVertex cx, boost::adjacent_vertices(vpp, g_))
    if (boost::edge(cx, v, g_).second && !boost::edge(cx, vp, g_).second)
      if (interfaceListsProperty_[vpp].interfaceHash[cx].size() > 0)
        Xs.push_back(cx);
  Xs.push_back(vpp);
}

bool SparseGraph::addPathToSpanner(const DensePath &densePath, SparseVertex vp, SparseVertex vpp)
{
  // First, check to see that the path has length
  if (densePath.size() <= 1)
  {
    // The path is 0 length, so simply link the representatives
    connectSparsePoints(vp, vpp);
  }
  else
  {
    // We will need to construct a PathGeometric to do this.
    smoothingGeomPath_.getStates().resize(densePath.size());
    std::copy(densePath.begin(), densePath.end(), smoothingGeomPath_.getStates().begin());

    // Attemp tto simplify the path
    psimp_->reduceVertices(smoothingGeomPath_, smoothingGeomPath_.getStateCount() * 2);

    // we are sure there are at least 2 points left on smoothingGeomPath_
    std::vector<SparseVertex> addedNodes;
    addedNodes.reserve(smoothingGeomPath_.getStateCount());
    for (std::size_t i = 0; i < smoothingGeomPath_.getStateCount(); ++i)
    {
      // Add each guard
      OMPL_ERROR("addVertex with state %u", smoothingGeomPath_.getState(i));
      exit(-1);
      // SparseVertex ng = addVertex(si_->cloneState(smoothingGeomPath_.getState(i)), QUALITY);
      // addedNodes.push_back(ng);
    }
    // Link them up
    for (std::size_t i = 1; i < addedNodes.size(); ++i)
    {
      connectSparsePoints(addedNodes[i - 1], addedNodes[i]);
    }
    // Don't forget to link them to their representatives
    connectSparsePoints(addedNodes[0], vp);
    connectSparsePoints(addedNodes[addedNodes.size() - 1], vpp);
  }
  smoothingGeomPath_.getStates().clear();
  return true;
}

void SparseGraph::connectSparsePoints(SparseVertex v, SparseVertex vp)
{
  OMPL_ERROR("connectSparsePoints");
  exit(-1);
  // const base::Cost weight(costHeuristic(v, vp));
  // const SpannerGraph::edge_property_type properties(weight);
  // boost::mutex::scoped_lock _(graphMutex_);
  // boost::add_edge(v, vp, properties, g_);
  // sparseDJSets_.union_set(v, vp);
}

TaskVertex SparseGraph::getInterfaceNeighbor(TaskVertex q, SparseVertex rep)
{
  foreach (TaskVertex vp, boost::adjacent_vertices(q, g_))
    if (denseDB_->representativesProperty_[vp] == rep)
      if (distanceFunction(q, vp) <= denseDelta_)
        return vp;
  throw Exception("SparseGraph", "Vertex has no interface neighbor with given representative");
}