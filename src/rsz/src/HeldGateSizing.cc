// SPDX-License-Identifier: BSD-3-Clause

#include "HeldGateSizing.hh"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

#include "rsz/Resizer.hh"
#include "sta/Corner.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Liberty.hh"
#include "sta/Network.hh"
#include "sta/PortDirection.hh"
#include "sta/Search.hh"
#include "sta/Units.hh"
#include "sta/Sta.hh"

namespace rsz {

using std::max;
using std::min;
using std::numeric_limits;
using std::sqrt;

using utl::RSZ;

using sta::dbStaState;
using sta::Graph;
using sta::LibertyCellSeq;
using sta::NetIterator;
using sta::InstancePinIterator;
using sta::PinConnectedPinIterator;
using sta::TimingArc;
using sta::TimingArcSet;
using sta::GateTimingModel;
using sta::MinMax;
using sta::ArcDelay;

HeldGateSizing::HeldGateSizing(Resizer* resizer,
                               double gamma,
                               double max_change,
                               int max_iterations)
    : resizer_(resizer),
      logger_(resizer->logger_),
      network_(static_cast<dbNetwork*>(resizer->network_)),
      sta_(resizer->sta_),
      gamma_(gamma),
      max_change_(max_change),
      max_iterations_(max_iterations),
      iteration_(0),
      endpoints_count_(0),
      best_objective_(numeric_limits<double>::infinity()),
      worst_slack_prev_(numeric_limits<double>::infinity()),
      objective_prev_(numeric_limits<double>::infinity()),
      default_max_slew_(0.5e-9),  // 500ps - more realistic
      default_min_slew_(10e-12),  // 10ps - reasonable minimum
      default_input_slew_(50e-12), // 50ps - reasonable default
      wire_slew_degrade_factor_(0.05), // Reduced from 0.1
      corner_(nullptr),
      dcalc_ap_(nullptr) {
}

bool HeldGateSizing::optimizeGateSizing(double clock_period) {
  clock_period_ = clock_period;
  
  // Find the slowest corner for sizing (similar to existing resizer logic)
  corner_ = resizer_->tgt_slew_corner_;
  if (!corner_) {
    // If no target slew corner, use the first available corner
    for (Corner* corner : *sta_->corners()) {
      corner_ = corner;
      break;
    }
  }
  
  if (!corner_) {
    // As last resort, create a default corner
    corner_ = sta_->findCorner("default");
    if (!corner_) {
      logger_->warn(RSZ, 100, "No timing corner found for gate sizing");
      return false;
    }
  }
  
  dcalc_ap_ = corner_->findDcalcAnalysisPt(MinMax::max());
  if (!dcalc_ap_) {
    logger_->warn(RSZ, 121, "No delay calculation analysis point found");
    return false;
  }
  
  logger_->info(RSZ, 101, "Starting Held's gate sizing algorithm");
  
  // Initialize the algorithm
  buildCellInfoMap();
  computeTopologicalLevels();
  initializeSlewTargets();
  
  // Save initial state as best
  saveCurrentAssignmentAsBest();
  
  // Main optimization loop
  bool improved = false;
  int stagnation_count = 0;
  const int max_stagnation = 3; // Stop if no improvement for 3 iterations
  
  for (iteration_ = 1; iteration_ <= max_iterations_; ++iteration_) {
    logger_->info(RSZ, 114, "Held sizing iteration {}", iteration_);
    
    // Step 1: Assign cells to meet slew targets (reverse topological order)
    assignCellsToMeetSlewTargets();
    
    // Step 2: Perform timing analysis
    performTimingAnalysis();
    
    // Step 3: Evaluate current solution
    double curr_worst_slack = computeWorstSlack();
    double curr_objective = computeObjective();
    
    logger_->info(RSZ, 115, 
                 "Iteration {} - WNS: {:.3f} ps, Objective: {:.6f}",
                 iteration_, 
                 curr_worst_slack * 1e12,
                 curr_objective);
    
    // Check if this is the best solution so far
    if (curr_objective < best_objective_ - 1e-6) { // Small improvement threshold
      best_objective_ = curr_objective;
      saveCurrentAssignmentAsBest();
      improved = true;
      stagnation_count = 0; // Reset stagnation counter
    } else {
      stagnation_count++;
    }
    
    // Step 4: Check stopping criteria
    if (iteration_ > 1) {
      // Check if algorithm is stagnating
      if (stagnation_count >= max_stagnation) {
        logger_->info(RSZ, 122, 
                     "Stopping - no improvement for {} iterations", 
                     max_stagnation);
        break;
      }
      
      // Check if the algorithm is diverging
      if (curr_worst_slack < worst_slack_prev_ && curr_objective > objective_prev_) {
        // WNS worsened and objective got worse - revert and stop
        restoreBestAssignment();
        logger_->info(RSZ, 123, 
                     "Stopping - reverted to best solution at iteration {}",
                     iteration_);
        break;
      }
      
      // Check for numerical convergence
      if (iteration_ > 2 && 
          abs(curr_worst_slack - worst_slack_prev_) < 1e-12 &&
          abs(curr_objective - objective_prev_) < 1e-9) {
        logger_->info(RSZ, 124, "Stopping - numerical convergence achieved");
        break;
      }
    }
    
    // Store current metrics for next iteration comparison
    worst_slack_prev_ = curr_worst_slack;
    objective_prev_ = curr_objective;
    
    // Step 5: Refine slew targets based on criticalities
    refineSlewTargets();
  }
  
  // Report final results
  double final_wns = computeWorstSlack();
  double final_tns = computeTotalNegativeSlack();
  double final_area = computeAverageCellArea();
  
  logger_->info(RSZ, 117,
               "Held sizing completed - WNS: {:.3f} ps, TNS: {:.3f} ps, "
               "Avg Area: {:.3f}, Iterations: {}",
               final_wns * 1e12,
               final_tns * 1e12, 
               final_area,
               iteration_);
  
  return improved;
}

void HeldGateSizing::buildCellInfoMap() {
  cell_info_map_.clear();
  
  int total_instances = 0;
  int sizeable_instances = 0;
  int multi_variant_instances = 0;
  
  // Iterate through all instances in the design
  sta::LeafInstanceIterator* inst_iter = network_->leafInstanceIterator();
  while (inst_iter->hasNext()) {
    Instance* inst = inst_iter->next();
    total_instances++;
    
    // Skip if this is not a standard cell or is dont_touch
    if (resizer_->dontTouch(inst) || !resizer_->isLogicStdCell(inst)) {
      continue;
    }
    
    LibertyCell* cell = network_->libertyCell(inst);
    if (!cell) continue;
    
    sizeable_instances++;
    
    HeldCellInfo cell_info;
    cell_info.instance = inst;
    cell_info.is_fixed = cell->hasSequentials(); // Registers are fixed
    cell_info.current_variant_idx = 0;
    cell_info.distance_from_src = 0.0;
    
    // Get equivalent cells (swappable variants)
    cell_info.variants = getEquivalentCells(inst);
    
    if (cell_info.variants.size() > 1) {
      multi_variant_instances++;
    }
    
    // Find current variant index
    for (size_t i = 0; i < cell_info.variants.size(); ++i) {
      if (cell_info.variants[i] == cell) {
        cell_info.current_variant_idx = i;
        break;
      }
    }
    
    // Initialize output pin data (assume single output for simplicity)
    InstancePinIterator* pin_iter = network_->pinIterator(inst);
    while (pin_iter->hasNext()) {
      Pin* pin = pin_iter->next();
      if (network_->direction(pin)->isOutput()) {
        cell_info.output.pin = pin;
        cell_info.output.slew_target = default_max_slew_;
        cell_info.output.wire_cap = 0.0;
        cell_info.output.downstream_cap = 0.0;
        
        // Collect sink pins
        PinConnectedPinIterator* conn_iter = network_->connectedPinIterator(pin);
        while (conn_iter->hasNext()) {
          const Pin* conn_pin = conn_iter->next();
          if (conn_pin != pin && network_->direction(conn_pin)->isInput()) {
            cell_info.output.sinks.push_back(const_cast<Pin*>(conn_pin));
          }
        }
        delete conn_iter;
        break; // Assume single output
      } else if (network_->direction(pin)->isInput()) {
        cell_info.input_pins.push_back(pin);
      }
    }
    delete pin_iter;
    
    cell_info_map_[inst] = cell_info;
  }
  delete inst_iter;
  
  logger_->info(RSZ, 118, "Built cell info map: {} total instances, {} sizeable, {} with multiple variants", 
               total_instances, sizeable_instances, multi_variant_instances);
}

std::vector<LibertyCell*> HeldGateSizing::getEquivalentCells(Instance* inst) {
  LibertyCell* cell = network_->libertyCell(inst);
  if (!cell) return {};
  
  // Use resizer's existing logic to get swappable cells
  LibertyCellSeq equiv_cells = resizer_->getSwappableCells(cell);
  std::vector<LibertyCell*> result(equiv_cells.begin(), equiv_cells.end());
  
  // Sort by drive strength (area as proxy)
  std::sort(result.begin(), result.end(), 
           [](const LibertyCell* a, const LibertyCell* b) {
             return a->area() < b->area();
           });
  
  return result;
}

void HeldGateSizing::computeTopologicalLevels() {
  // Use OpenROAD's existing level computation infrastructure
  resizer_->ensureLevelDrvrVertices();
  
  // Convert to our data structure and sort
  topo_sorted_cells_.clear();
  for (auto& [inst, cell_info] : cell_info_map_) {
    // Find the instance's driver vertex to get its level
    Pin* drvr_pin = cell_info.output.pin;
    if (drvr_pin) {
      sta::Vertex* vertex = resizer_->graph_->pinDrvrVertex(drvr_pin);
      if (vertex) {
        cell_info.distance_from_src = vertex->level();
      }
    }
    topo_sorted_cells_.push_back(&cell_info);
  }
  
  // Sort by topological level (distance from source)
  std::sort(topo_sorted_cells_.begin(), topo_sorted_cells_.end(),
           [](const HeldCellInfo* a, const HeldCellInfo* b) {
             return a->distance_from_src < b->distance_from_src;
           });
}

void HeldGateSizing::initializeSlewTargets() {
  // Initialize slew targets based on actual constraints and technology characteristics
  for (auto& [inst, cell_info] : cell_info_map_) {
    if (cell_info.output.pin) {
      double min_sink_limit = default_max_slew_;
      
      // Find minimum slew limit among all sinks
      bool found_constraint = false;
      for (Pin* sink_pin : cell_info.output.sinks) {
        LibertyPort* sink_port = network_->libertyPort(sink_pin);
        if (sink_port) {
          float slew_limit;
          bool exists;
          sta_->findSlewLimit(sink_port, corner_, MinMax::max(), slew_limit, exists);
          if (exists && slew_limit > 0) {
            min_sink_limit = min(min_sink_limit, (double)slew_limit);
            found_constraint = true;
          }
        }
      }
      
      // If no explicit constraints found, use technology-appropriate defaults
      if (!found_constraint) {
        // Use more conservative default based on fanout
        int fanout = cell_info.output.sinks.size();
        if (fanout == 0) {
          min_sink_limit = default_max_slew_ * 0.5; // Tighter for outputs
        } else if (fanout <= 4) {
          min_sink_limit = default_max_slew_ * 0.6; // Normal fanout
        } else {
          min_sink_limit = default_max_slew_ * 0.8; // High fanout, relax slightly
        }
      }
      
      // Account for wire degradation more realistically
      double load_cap = resizer_->graph_delay_calc_->loadCap(cell_info.output.pin, dcalc_ap_);
      double wire_degradation = load_cap * 1e12 * wire_slew_degrade_factor_; // Convert to ps
      double initial_target = min_sink_limit - wire_degradation * 1e-12; // Convert back to seconds
      
      // More conservative bounds
      initial_target = max(initial_target, default_min_slew_);
      initial_target = min(initial_target, min_sink_limit * 0.9); // Stay below limit
      
      cell_info.output.slew_target = initial_target;
    }
  }
}

void HeldGateSizing::assignCellsToMeetSlewTargets() {
  // Process cells in reverse topological order (as in Held's algorithm)
  int cells_changed = 0;
  int total_cells_processed = 0;
  int cells_with_variants = 0;
  
  // Get current worst slack for prioritization
  double current_wns = sta_->worstSlack(MinMax::max());
  
  for (auto it = topo_sorted_cells_.rbegin(); it != topo_sorted_cells_.rend(); ++it) {
    HeldCellInfo* cell_info = *it;
    total_cells_processed++;
    
    if (cell_info->is_fixed) {
      continue; // Skip fixed cells (registers)
    }

    Pin* output_pin = cell_info->output.pin;
    if (!output_pin || cell_info->variants.empty()) continue;
    
    if (cell_info->variants.size() > 1) {
      cells_with_variants++;
    }
    
    // Calculate current load capacitance
    double load_cap = resizer_->graph_delay_calc_->loadCap(output_pin, dcalc_ap_);
    cell_info->output.downstream_cap = load_cap;
    
    // Skip if no variants to choose from
    if (cell_info->variants.size() <= 1) {
      continue;
    }
    
    // Get current slack for this pin
    double pin_slack = sta_->pinSlack(output_pin, MinMax::max());
    
    // Estimate input slew more accurately
    double input_slew = estimateInputSlew(cell_info->input_pins.empty() ? nullptr : cell_info->input_pins[0]);
    
    // Find the best variant considering both timing and slew targets
    int best_idx = cell_info->current_variant_idx;
    double best_score = numeric_limits<double>::infinity();
    
    for (size_t i = 0; i < cell_info->variants.size(); ++i) {
      LibertyCell* variant = cell_info->variants[i];
      
      // Compute output slew for this variant
      double output_slew = computeOutputSlew(variant, input_slew, load_cap);
      
      // Estimate delay improvement (simplified)
      double area_ratio = variant->area() / cell_info->variants[cell_info->current_variant_idx]->area();
      double delay_estimate = input_slew * 0.5 + load_cap * 1e12 / area_ratio; // Simplified delay model
      
      // Multi-objective scoring:
      // 1. Slew target satisfaction
      double slew_error = abs(output_slew - cell_info->output.slew_target);
      
      // 2. Timing criticality consideration - be more conservative
      double timing_weight = 1.0;
      if (pin_slack < 0) {
        // Critical path - heavily prioritize timing improvement
        timing_weight = (area_ratio > 1.0) ? 50.0 : 1.0; // Favor upsizing on critical paths
      } else if (pin_slack > abs(current_wns) * 0.2) {
        // Non-critical path - allow area optimization
        timing_weight = (area_ratio < 1.0) ? 0.5 : 10.0; // Favor downsizing on non-critical paths
      } else {
        // Marginal slack - be very conservative
        timing_weight = 100.0; // Discourage changes
      }
      
      // 3. Area penalty/benefit
      double area_factor = 1.0;
      if (pin_slack < 0) {
        // Critical: prefer larger cells (lower resistance)
        area_factor = 1.0 / sqrt(area_ratio);
      } else {
        // Non-critical: prefer smaller cells (save area)
        area_factor = sqrt(area_ratio);
      }
      
      // Combined score (lower is better)
      double score = slew_error * timing_weight + delay_estimate * 0.01 + area_factor * 0.1;
      
      if (score < best_score) {
        best_score = score;
        best_idx = i;
      }
    }
    
    // Only apply changes that seem beneficial and aren't too risky
    if (best_idx != cell_info->current_variant_idx) {
      LibertyCell* best_cell = cell_info->variants[best_idx];
      LibertyCell* current_cell = network_->libertyCell(cell_info->instance);
      
      // Additional safety check: don't make dramatic changes on critical paths
      if (pin_slack < 0) {
        double size_change = best_cell->area() / current_cell->area();
        if (size_change < 0.7 || size_change > 2.0) {
          continue; // Skip dramatic size changes on critical paths
        }
      }
      
      if (best_cell != current_cell) {
        if (resizer_->replaceCell(cell_info->instance, best_cell, true)) {
          cell_info->current_variant_idx = best_idx;
          cells_changed++;
        }
      }
    }
  }
  
  if (iteration_ <= 3) {  // Debug for first few iterations
    logger_->info(RSZ, 127, "Assignment phase: processed {} cells, {} with variants, changed {} cells", 
                 total_cells_processed, cells_with_variants, cells_changed);
  }
}

void HeldGateSizing::performTimingAnalysis() {
  // Use OpenROAD's STA to update timing
  sta_->findDelays();
  
  // Update our data structures with timing results
  endpoints_count_ = 0;
  for (auto& [inst, cell_info] : cell_info_map_) {
    if (cell_info.output.pin) {
      Pin* pin = cell_info.output.pin;
      
      // Get arrival and required times
      sta::Vertex* vertex = resizer_->graph_->pinDrvrVertex(pin);
      if (vertex) {
        cell_info.output.arrival_time = sta_->pinArrival(pin, RiseFall::rise(), 
                                                        MinMax::max());
        cell_info.output.required_time = sta_->vertexRequired(vertex, RiseFall::rise(),
                                                          MinMax::max());
        cell_info.output.slack = cell_info.output.required_time - 
                                cell_info.output.arrival_time;
        
        // Get actual slew
        cell_info.output.slew_actual = sta_->vertexSlew(vertex, RiseFall::rise(),
                                                        MinMax::max());
      }
      
      // Count endpoints (register inputs or primary outputs)
      if (cell_info.is_fixed || cell_info.output.sinks.empty()) {
        endpoints_count_++;
      }
    }
  }
}

void HeldGateSizing::refineSlewTargets() {
  // More aggressive slew target refinement based on timing criticality
  int target_changes = 0;
  double total_delta = 0.0;
  
  // Get current worst slack to drive the algorithm
  double current_wns = sta_->worstSlack(MinMax::max());
  if (current_wns >= 0) current_wns = -1e-12; // Avoid division by zero
  
  for (auto& [inst, cell_info] : cell_info_map_) {
    if (!cell_info.output.pin) continue;
    
    double old_target = cell_info.output.slew_target;
    
    // Get current slack for this pin directly from STA
    double pin_slack = sta_->pinSlack(cell_info.output.pin, MinMax::max());
    
    // More aggressive adjustment based on criticality
    double delta = 0.0;
    
    if (pin_slack < 0) {
      // Critical path - tighten slew target aggressively to improve timing
      double criticality = abs(pin_slack) / abs(current_wns);  // 0 to 1+
      criticality = min(criticality, 2.0); // Cap at 2x worst slack
      
      // More aggressive scaling for critical paths
      double aggressiveness = (iteration_ <= 2) ? 1.0 : (0.8 / log(iteration_));
      aggressiveness = max(0.3, min(1.0, aggressiveness));
      
      // Tighten slew target significantly on critical paths
      delta = -gamma_ * criticality * aggressiveness * max_change_ * 2.0;
      
    } else if (pin_slack > abs(current_wns) * 0.1) {
      // Non-critical path with decent slack - relax slew target to save area
      double slack_margin = min(pin_slack / abs(current_wns), 3.0);
      
      // Less aggressive relaxation
      double relax_factor = (iteration_ <= 2) ? 0.5 : (0.3 / log(iteration_));
      relax_factor = max(0.1, min(0.5, relax_factor));
      
      // Relax slew target on non-critical paths
      delta = gamma_ * slack_margin * relax_factor * max_change_;
    }
    // For slightly critical paths (small positive slack), don't change much
    
    // Update slew target with bounds checking
    double new_target = cell_info.output.slew_target + delta;
    
    // More realistic bounds based on technology (tighter range for better control)
    double min_practical = default_min_slew_;
    double max_practical = default_max_slew_ * 0.8; // Tighter upper bound
    
    // Clamp to practical range
    new_target = max(new_target, min_practical);
    new_target = min(new_target, max_practical);
    
    cell_info.output.slew_target = new_target;
    
    if (abs(new_target - old_target) > 1e-15) {
      target_changes++;
      total_delta += abs(new_target - old_target);
    }
  }
  
  if (iteration_ <= 3) {  // Debug for first few iterations
    logger_->info(RSZ, 128, "Refinement phase: {} targets changed, avg delta: {:.3e} s, WNS: {:.3f} ps",
                 target_changes, target_changes > 0 ? total_delta/target_changes : 0.0,
                 current_wns * 1e12);
  }
}

double HeldGateSizing::computeOutputSlew(LibertyCell* cell,
                                        double input_slew,
                                        double load_cap) {
  // Use OpenROAD's existing gate delay calculation infrastructure
  LibertyPort* output_port = nullptr;
  sta::LibertyCellPortIterator port_iter(cell);
  while (port_iter.hasNext()) {
    LibertyPort* port = port_iter.next();
    if (port->direction()->isOutput()) {
      output_port = port;
      break;
    }
  }
  
  if (!output_port) return default_max_slew_;
  
  // Use resizer's gate delay calculation
  ArcDelay delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  resizer_->gateDelays(output_port, load_cap, dcalc_ap_, delays, slews);
  
  return max(slews[RiseFall::riseIndex()], slews[RiseFall::fallIndex()]);
}

double HeldGateSizing::estimateInputSlew(Pin* input_pin) {
  if (!input_pin) {
    // Use worst case among all input pins if none specified
    double worst_slew = default_input_slew_;
    for (auto& [inst, cell_info] : cell_info_map_) {
      for (Pin* pin : cell_info.input_pins) {
        sta::Vertex* vertex = resizer_->graph_->pinLoadVertex(pin);
        if (vertex) {
          double pin_slew = sta_->vertexSlew(vertex, RiseFall::rise(), MinMax::max());
          worst_slew = max(worst_slew, pin_slew);
        }
      }
    }
    return worst_slew;
  }
  
  // Find driving pin and use its actual slew
  Pin* driving_pin = nullptr;
  PinConnectedPinIterator* conn_iter = this->network_->connectedPinIterator(input_pin);
  while (conn_iter->hasNext()) {
    const Pin* conn_pin = conn_iter->next();
    if (conn_pin != input_pin && this->network_->direction(conn_pin)->isOutput()) {
      driving_pin = const_cast<Pin*>(conn_pin);
      break;
    }
  }
  delete conn_iter;
  
  if (driving_pin) {
    // Get actual slew from STA
    sta::Vertex* drvr_vertex = resizer_->graph_->pinDrvrVertex(driving_pin);
    if (drvr_vertex) {
      double actual_slew = sta_->vertexSlew(drvr_vertex, RiseFall::rise(), MinMax::max());
      
      // If we have cell info for the driving instance, use mix of target and actual
      Instance* driving_inst = this->network_->instance(driving_pin);
      auto it = cell_info_map_.find(driving_inst);
      if (it != cell_info_map_.end()) {
        // Weight actual slew more heavily in later iterations
        double actual_weight = (iteration_ <= 1) ? 0.3 : min(0.8, 0.3 + iteration_ * 0.1);
        double target_weight = 1.0 - actual_weight;
        return actual_weight * actual_slew + target_weight * it->second.output.slew_target;
      } else {
        // Just use actual slew if we don't track this cell
        return actual_slew;
      }
    }
  }
  
  return default_input_slew_;
}

double HeldGateSizing::computeSlack(Pin* pin) {
  return this->sta_->pinSlack(pin, MinMax::max());
}

double HeldGateSizing::computeWorstSlack() {
  // Use OpenROAD's STA to get the actual worst slack
  return sta_->worstSlack(MinMax::max());
}

double HeldGateSizing::computeTotalNegativeSlack() {
  // Use OpenROAD's STA to get the actual total negative slack
  return sta_->totalNegativeSlack(MinMax::max());
}

double HeldGateSizing::computeAverageCellArea() {
  if (cell_info_map_.empty()) return 0.0;
  
  double total_area = 0.0;
  for (const auto& [inst, cell_info] : cell_info_map_) {
    LibertyCell* cell = this->network_->libertyCell(inst);
    if (cell) {
      total_area += cell->area();
    }
  }
  
  return total_area / cell_info_map_.size();
}

double HeldGateSizing::computeObjective() {
  double wns = computeWorstSlack();
  double tns = computeTotalNegativeSlack();
  double avg_area = computeAverageCellArea();
  
  // Normalize metrics for better balance
  double base_area = 1.0; // Assume base area of 1.0 as reference
  double area_ratio = avg_area / base_area;
  
  // Timing-focused objective with area penalty
  if (wns < 0) {
    // When timing is violated, heavily weight timing
    // WNS in ps (absolute value), TNS normalized by endpoint count
    double wns_ps = abs(wns * 1e12);
    double tns_per_endpoint = (endpoints_count_ > 0) ? abs(tns * 1e12) / endpoints_count_ : 0.0;
    
    // Primary objective: fix timing violations
    // Secondary objective: minimize area growth
    return wns_ps * 100.0 + tns_per_endpoint * 10.0 + area_ratio * 1.0;
  } else {
    // When timing is met, focus on area optimization
    // Small timing bonus to maintain positive slack
    double slack_bonus = min(wns * 1e12, 100.0); // Cap bonus at 100ps
    return area_ratio * 100.0 - slack_bonus;
  }
}

void HeldGateSizing::saveCurrentAssignmentAsBest() {
  best_assignment_.clear();
  for (const auto& [inst, cell_info] : cell_info_map_) {
    best_assignment_[inst] = cell_info.current_variant_idx;
  }
}

void HeldGateSizing::restoreBestAssignment() {
  for (auto& [inst, cell_info] : cell_info_map_) {
    auto it = best_assignment_.find(inst);
    if (it != best_assignment_.end()) {
      int best_idx = it->second;
      if (best_idx != cell_info.current_variant_idx && 
          best_idx < (int)cell_info.variants.size()) {
        LibertyCell* best_cell = cell_info.variants[best_idx];
        LibertyCell* current_cell = this->network_->libertyCell(inst);
        if (best_cell != current_cell) {
          resizer_->replaceCell(inst, best_cell, true);
          cell_info.current_variant_idx = best_idx;
        }
      }
    }
  }
  
  // Rerun timing analysis
  performTimingAnalysis();
}

void HeldGateSizing::setDefaultSlewLimits(double max_slew, double min_slew) {
  default_max_slew_ = max_slew;
  default_min_slew_ = min_slew;
}

}  // namespace rsz