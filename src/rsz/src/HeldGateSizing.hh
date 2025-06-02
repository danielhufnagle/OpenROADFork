// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <array>
#include <limits>
#include <unordered_map>
#include <vector>

#include "db_sta/dbNetwork.hh"
#include "db_sta/dbSta.hh"
#include "sta/Liberty.hh"
#include "sta/Network.hh"
#include "sta/Sta.hh"
#include "sta/PortDirection.hh"
#include "utl/Logger.h"

namespace rsz {

using utl::Logger;

using sta::Corner;
using sta::DcalcAnalysisPt;
using sta::dbNetwork;
using sta::dbSta;
using sta::Instance;
using sta::LibertyCell;
using sta::Net;
using sta::Pin;
using sta::RiseFall;
using sta::Slew;

class Resizer;

// Structure to represent a cell instance in Held's algorithm
struct HeldCellInfo {
  Instance* instance;                      // OpenROAD instance
  std::vector<LibertyCell*> variants;     // Available equivalent cells
  int current_variant_idx;                 // Current variant index
  bool is_fixed;                          // True for registers/fixed cells
  double distance_from_src;               // Topological level
  
  // Timing data for this cell's output
  struct OutputData {
    Pin* pin;                             // Output pin
    double slew_target;                   // Current slew target
    double slew_actual;                   // Actual slew from STA
    double arrival_time;                  // Arrival time
    double required_time;                 // Required time
    double slack;                         // Slack = required - arrival
    double wire_cap;                      // Wire capacitance
    double downstream_cap;                // Total load capacitance
    std::vector<Pin*> sinks;             // Sink pins driven by this output
  } output;
  
  // Input pins data
  std::vector<Pin*> input_pins;
};

// Held's fast global gate sizing algorithm implementation
// Integrated with OpenROAD's resizer infrastructure
class HeldGateSizing {
public:
  HeldGateSizing(Resizer* resizer,
                 double gamma = 0.5,
                 double max_change = 0.05,
                 int max_iterations = 50);
  ~HeldGateSizing() = default;

  // Main algorithm entry point
  bool optimizeGateSizing(double clock_period);

  // Configuration
  void setGamma(double gamma) { gamma_ = gamma; }
  void setMaxChange(double max_change) { max_change_ = max_change; }
  void setMaxIterations(int max_iters) { max_iterations_ = max_iters; }
  void setDefaultSlewLimits(double max_slew, double min_slew = 0.0);

private:
  // Algorithm phases
  void initializeSlewTargets();
  void assignCellsToMeetSlewTargets();
  void performTimingAnalysis();
  void refineSlewTargets();
  
  // Helper functions
  void buildCellInfoMap();
  void computeTopologicalLevels();
  std::vector<LibertyCell*> getEquivalentCells(Instance* inst);
  double computeOutputSlew(LibertyCell* cell, 
                          double input_slew, 
                          double load_cap);
  double estimateInputSlew(Pin* input_pin);
  double computeSlack(Pin* pin);
  
  // Objective evaluation
  double computeWorstSlack();
  double computeTotalNegativeSlack();
  double computeAverageCellArea();
  double computeObjective();
  
  // Best solution tracking
  void saveCurrentAssignmentAsBest();
  void restoreBestAssignment();
  
  // OpenROAD integration
  Resizer* resizer_;
  Logger* logger_;
  dbNetwork* network_;
  dbSta* sta_;
  
  // Algorithm parameters
  double clock_period_;
  double gamma_;                          // Tuning parameter for slew target adjustment
  double max_change_;                     // Maximum allowed slew target change per iteration
  int max_iterations_;
  
  // Current state
  int iteration_;
  int endpoints_count_;
  std::unordered_map<Instance*, HeldCellInfo> cell_info_map_;
  std::vector<HeldCellInfo*> topo_sorted_cells_;
  
  // Best solution tracking
  std::unordered_map<Instance*, int> best_assignment_;
  double best_objective_;
  double worst_slack_prev_;
  double objective_prev_;
  
  // Default values
  double default_max_slew_;
  double default_min_slew_;
  double default_input_slew_;
  double wire_slew_degrade_factor_;
  
  // Timing corners
  const Corner* corner_;
  const DcalcAnalysisPt* dcalc_ap_;
};

}  // namespace rsz 