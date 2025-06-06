// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2023-2025, The OpenROAD Authors

#include "RecoverPower.hh"

#include <algorithm>
#include <string>

#include "db_sta/dbNetwork.hh"
#include "rsz/Resizer.hh"
#include "sta/Corner.hh"
#include "sta/DcalcAnalysisPt.hh"
#include "sta/Fuzzy.hh"
#include "sta/Graph.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/InputDrive.hh"
#include "sta/Liberty.hh"
#include "sta/Parasitics.hh"
#include "sta/PathExpanded.hh"
#include "sta/PortDirection.hh"
#include "sta/Sdc.hh"
#include "sta/TimingArc.hh"
#include "sta/Units.hh"
#include "utl/Logger.h"

namespace rsz {

using std::pair;
using std::string;
using std::vector;

using utl::RSZ;

using sta::Edge;
using sta::PathExpanded;
using sta::VertexOutEdgeIterator;

RecoverPower::RecoverPower(Resizer* resizer)
    : resizer_(resizer), bad_vertices_(resizer->graph_)
{
}

void RecoverPower::init()
{
  logger_ = resizer_->logger_;
  dbStaState::init(resizer_->sta_);
  db_network_ = resizer_->db_network_;
  initial_design_area_ = resizer_->computeDesignArea();
}

bool RecoverPower::recoverPower(const float recover_power_percent, bool verbose)
{
  bool recovered = false;
  init();
  constexpr int digits = 3;
  resize_count_ = 0;
  resizer_->buffer_moved_into_core_ = false;

  // Sort failing endpoints by slack.
  VertexSet* endpoints = sta_->endpoints();
  VertexSeq ends_with_slack;
  for (Vertex* end : *endpoints) {
    const Slack end_slack = sta_->vertexSlack(end, max_);
    if (end_slack > setup_slack_margin_
        && end_slack < setup_slack_max_margin_) {
      ends_with_slack.push_back(end);
    }
  }

  sort(ends_with_slack, [=](Vertex* end1, Vertex* end2) {
    return sta_->vertexSlack(end1, max_) > sta_->vertexSlack(end2, max_);
  });

  debugPrint(logger_,
             RSZ,
             "recover_power",
             1,
             "Candidate paths {}/{} {}%",
             ends_with_slack.size(),
             endpoints->size(),
             int(ends_with_slack.size() / double(endpoints->size()) * 100));

  int max_end_count = ends_with_slack.size() * recover_power_percent;
  // As long as we are here fix at least one path
  max_end_count = std::max(max_end_count, 1);

  Slack worst_slack_before;
  Vertex* worst_vertex;
  sta_->worstSlack(max_, worst_slack_before, worst_vertex);

  if (max_end_count > 5 * max_print_interval_) {
    print_interval_ = max_print_interval_;
  } else {
    print_interval_ = min_print_interval_;
  }

  printProgress(0, false, false);

  int end_index = 0;
  int failed_move_threshold = 0;
  for (Vertex* end : ends_with_slack) {
    resizer_->journalBegin();
    const Slack end_slack_before = sta_->vertexSlack(end, max_);
    Slack worst_slack_after;
    //=====================================================================
    // Just a counter to know when to break out
    end_index++;
    debugPrint(logger_,
               RSZ,
               "recover_power",
               2,
               "Doing {} / {}",
               end_index,
               max_end_count);
    if (verbose || end_index == 1) {
      printProgress(end_index, false, false);
    }

    if (end_index > max_end_count) {
      resizer_->journalEnd();
      break;
    }
    //=====================================================================
    Path* end_path = sta_->vertexWorstSlackPath(end, max_);
    Vertex* const changed = recoverPower(end_path, end_slack_before);
    if (changed) {
      resizer_->updateParasitics(true);
      sta_->findRequireds();
      const Slack end_slack_after = sta_->vertexSlack(end, max_);

      sta_->worstSlack(max_, worst_slack_after, worst_vertex);

      const float worst_slack_percent = fabs(
          (worst_slack_before - worst_slack_after) / worst_slack_before * 100);
      const bool better
          = (worst_slack_percent < 0.0001
             || (worst_slack_before > 0
                 && worst_slack_after / worst_slack_before > 0.5));

      debugPrint(logger_,
                 RSZ,
                 "recover_power",
                 2,
                 "slack = {} worst_slack = {} better = {}",
                 delayAsString(end_slack_after, sta_, digits),
                 delayAsString(worst_slack_after, sta_, digits),
                 better ? "save" : "");

      if (better) {
        failed_move_threshold = 0;
        resizer_->journalEnd();
        resizer_->journalBegin();
        debugPrint(logger_,
                   RSZ,
                   "recover_power",
                   2,
                   "{}/{} Resize for power Slack change {} -> {}",
                   end_index,
                   ends_with_slack.size(),
                   worst_slack_before,
                   worst_slack_after);
      } else {
        // Save the vertex to avoid trying it again.
        bad_vertices_.insert(changed);
        // Undo the change here.
        ++failed_move_threshold;
        if (failed_move_threshold > failed_move_threshold_limit_) {
          logger_->info(RSZ,
                        142,
                        "{} successive tries yielded negative slack. Ending "
                        "power recovery",
                        failed_move_threshold_limit_);
          resizer_->journalEnd();
          break;
        }
        resizer_->journalRestore();
        debugPrint(logger_,
                   RSZ,
                   "recover_power",
                   2,
                   "{}/{} Undo resize for power Slack change {} -> {}",
                   end_index,
                   ends_with_slack.size(),
                   worst_slack_before,
                   worst_slack_after);
      }
      if (resizer_->overMaxArea()) {
        resizer_->journalEnd();
        break;
      }
    }
  }

  printProgress(end_index, true, true);

  bad_vertices_.clear();

  // TODO: Add the appropriate metric here
  // logger_->metric("design__instance__count__setup_buffer",
  // inserted_buffer_count_);
  if (resize_count_ > 0) {
    recovered = true;
    logger_->info(RSZ, 141, "Resized {} instances.", resize_count_);
  }
  if (resizer_->overMaxArea()) {
    logger_->error(RSZ, 125, "max utilization reached.");
  }

  return recovered;
}

// For testing.
Vertex* RecoverPower::recoverPower(const Pin* end_pin)
{
  init();
  resize_count_ = 0;

  Vertex* vertex = graph_->pinLoadVertex(end_pin);
  const Slack slack = sta_->vertexSlack(vertex, max_);
  const Path* path = sta_->vertexWorstSlackPath(vertex, max_);
  resizer_->incrementalParasiticsBegin();
  Vertex* drvr_vertex = recoverPower(path, slack);
  // Leave the parasitices up to date.
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (resize_count_ > 0) {
    logger_->info(RSZ, 3111, "Resized {} instances.", resize_count_);
  }
  return drvr_vertex;
}

// This is the main routine for recovering power.
Vertex* RecoverPower::recoverPower(const Path* path, const Slack path_slack)
{
  PathExpanded expanded(path, sta_);
  Vertex* changed = nullptr;

  if (expanded.size() > 1) {
    const int path_length = expanded.size();
    vector<pair<int, Delay>> load_delays;
    const int start_index = expanded.startIndex();
    const DcalcAnalysisPt* dcalc_ap = path->dcalcAnalysisPt(sta_);
    const int lib_ap = dcalc_ap->libertyIndex();
    // Find load delay for each gate in the path.
    for (int i = start_index; i < path_length; i++) {
      const Path* path = expanded.path(i);
      const Vertex* path_vertex = path->vertex(sta_);
      const Pin* path_pin = path->pin(sta_);
      if (i > 0 && network_->isDriver(path_pin)
          && !network_->isTopLevelPort(path_pin)) {
        const TimingArc* prev_arc = path->prevArc(sta_);
        const TimingArc* corner_arc = prev_arc->cornerArc(lib_ap);
        const Edge* prev_edge = path->prevEdge(sta_);
        const Delay load_delay
            = graph_->arcDelay(prev_edge, prev_arc, dcalc_ap->index())
              // Remove intrinsic delay to find load dependent delay.
              - corner_arc->intrinsicDelay();
        load_delays.emplace_back(i, load_delay);
        debugPrint(logger_,
                   RSZ,
                   "recover_power",
                   3,
                   "{} load_delay = {}",
                   path_vertex->name(network_),
                   delayAsString(load_delay, sta_, 3));
      }
    }

    // Sort the delays for any specific path. This way we can pick the fastest
    // delay and downsize that cell to achieve our goal instead of messing with
    // too many cells.
    sort(
        load_delays.begin(),
        load_delays.end(),
        [](const pair<int, Delay>& pair1, const pair<int, Delay>& pair2) {
          return pair1.second > pair2.second
                 || (pair1.second == pair2.second && pair1.first < pair2.first);
        });
    for (const auto& [drvr_index, ignored] : load_delays) {
      const Path* drvr_path = expanded.path(drvr_index);
      Vertex* drvr_vertex = drvr_path->vertex(sta_);
      // If we already tried this vertex and got a worse result, skip it.
      if (bad_vertices_.find(drvr_vertex) != bad_vertices_.end()) {
        continue;
      }
      const Pin* drvr_pin = drvr_vertex->pin();
      const LibertyPort* drvr_port = network_->libertyPort(drvr_pin);
      const LibertyCell* drvr_cell
          = drvr_port ? drvr_port->libertyCell() : nullptr;
      const int fanout = this->fanout(drvr_vertex);
      debugPrint(logger_,
                 RSZ,
                 "recover_power",
                 3,
                 "{} {} fanout = {}",
                 network_->pathName(drvr_pin),
                 drvr_cell ? drvr_cell->name() : "none",
                 fanout);
      if (downsizeDrvr(drvr_path, drvr_index, &expanded, true, path_slack)) {
        changed = drvr_vertex;
        break;
      }
    }
  }
  return changed;
}

bool RecoverPower::downsizeDrvr(const Path* drvr_path,
                                const int drvr_index,
                                PathExpanded* expanded,
                                const bool only_same_size_swap,
                                const Slack path_slack)
{
  const Pin* drvr_pin = drvr_path->pin(this);
  Instance* drvr = network_->instance(drvr_pin);
  const DcalcAnalysisPt* dcalc_ap = drvr_path->dcalcAnalysisPt(sta_);
  const float load_cap = graph_delay_calc_->loadCap(drvr_pin, dcalc_ap);
  const int in_index = drvr_index - 1;
  const Path* in_path = expanded->path(in_index);
  const Pin* in_pin = in_path->pin(sta_);
  const LibertyPort* in_port = network_->libertyPort(in_pin);
  if (!resizer_->dontTouch(drvr)) {
    float prev_drive = 0.0;
    if (drvr_index >= 2) {
      const int prev_drvr_index = drvr_index - 2;
      const Path* prev_drvr_path = expanded->path(prev_drvr_index);
      const Pin* prev_drvr_pin = prev_drvr_path->pin(sta_);
      const LibertyPort* prev_drvr_port = network_->libertyPort(prev_drvr_pin);
      if (prev_drvr_port) {
        prev_drive = prev_drvr_port->driveResistance();
      }
    }
    const LibertyPort* drvr_port = network_->libertyPort(drvr_pin);
    const LibertyCell* downsize = downsizeCell(in_port,
                                               drvr_port,
                                               load_cap,
                                               prev_drive,
                                               dcalc_ap,
                                               only_same_size_swap,
                                               path_slack);
    if (downsize != nullptr) {
      debugPrint(logger_,
                 RSZ,
                 "recover_power",
                 3,
                 "resize {} {} -> {}",
                 network_->pathName(drvr_pin),
                 drvr_port->libertyCell()->name(),
                 downsize->name());
      if (resizer_->replaceCell(drvr, downsize, true)) {
        resize_count_++;
        return true;
      }
    }
  }
  return false;
}

bool RecoverPower::meetsSizeCriteria(const LibertyCell* cell,
                                     const LibertyCell* candidate,
                                     const bool match_size)
{
  if (!match_size) {
    return true;
  }
  const dbMaster* candidate_cell = db_network_->staToDb(candidate);
  const dbMaster* curr_cell = db_network_->staToDb(cell);
  if (candidate_cell->getWidth() <= curr_cell->getWidth()
      && candidate_cell->getHeight() == curr_cell->getHeight()) {
    return true;
  }
  return false;
}

LibertyCell* RecoverPower::downsizeCell(const LibertyPort* in_port,
                                        const LibertyPort* drvr_port,
                                        const float load_cap,
                                        const float prev_drive,
                                        const DcalcAnalysisPt* dcalc_ap,
                                        const bool match_size,
                                        const Slack path_slack)
{
  const int lib_ap = dcalc_ap->libertyIndex();
  LibertyCell* cell = drvr_port->libertyCell();
  LibertyCellSeq swappable_cells = resizer_->getSwappableCells(cell);
  constexpr double delay_margin = 1.5;  // Prevent overly aggressive downsizing

  if (!swappable_cells.empty()) {
    const char* in_port_name = in_port->name();
    const char* drvr_port_name = drvr_port->name();
    sort(&swappable_cells,
         [=](const LibertyCell* cell1, const LibertyCell* cell2) {
           LibertyPort* port1
               = cell1->findLibertyPort(drvr_port_name)->cornerPort(lib_ap);
           const LibertyPort* port2
               = cell2->findLibertyPort(drvr_port_name)->cornerPort(lib_ap);
           const float drive1 = port1->driveResistance();
           const float drive2 = port2->driveResistance();
           const ArcDelay intrinsic1 = port1->intrinsicDelay(this);
           const ArcDelay intrinsic2 = port2->intrinsicDelay(this);
           return (std::tie(drive1, intrinsic2) < std::tie(drive2, intrinsic1));
         });
    const float drive = drvr_port->cornerPort(lib_ap)->driveResistance();
    const float delay
        = resizer_->gateDelay(drvr_port, load_cap, resizer_->tgt_slew_dcalc_ap_)
          + prev_drive * in_port->cornerPort(lib_ap)->capacitance();

    LibertyCell* best_cell = nullptr;
    for (LibertyCell* swappable : swappable_cells) {
      const LibertyCell* swappable_corner = swappable->cornerCell(lib_ap);
      const LibertyPort* swappable_drvr
          = swappable_corner->findLibertyPort(drvr_port_name);
      const LibertyPort* swappable_input
          = swappable_corner->findLibertyPort(in_port_name);
      const float current_drive = swappable_drvr->driveResistance();
      // Include delay of previous driver into swappable gate.
      const float current_delay
          = resizer_->gateDelay(swappable_drvr, load_cap, dcalc_ap)
            + prev_drive * swappable_input->capacitance();

      if (!resizer_->dontUse(swappable) && current_drive > drive
          && current_delay > delay
          && (current_delay - delay) * delay_margin < path_slack  // add margin
          && meetsSizeCriteria(cell, swappable, match_size)) {
        best_cell = swappable;
      }
    }
    if (best_cell != nullptr) {
      return best_cell;
    }
  }
  return nullptr;
}

int RecoverPower::fanout(Vertex* vertex)
{
  int fanout = 0;
  VertexOutEdgeIterator edge_iter(vertex, graph_);
  while (edge_iter.hasNext()) {
    edge_iter.next();
    fanout++;
  }
  return fanout;
}

void RecoverPower::printProgress(int iteration, bool force, bool end) const
{
  const bool start = iteration == 0;

  if (start && !end) {
    logger_->report("Iteration |   Area    |  Resized |   WNS    | Endpt");
    logger_->report("---------------------------------------------------");
  }

  if (iteration % print_interval_ == 0 || force || end) {
    Slack wns;
    Vertex* worst_vertex;
    sta_->worstSlack(max_, wns, worst_vertex);

    std::string itr_field = fmt::format("{}", iteration);
    if (end) {
      itr_field = "final";
    }

    const double design_area = resizer_->computeDesignArea();
    const double area_growth = design_area - initial_design_area_;

    logger_->report(
        "{: >9s} | {: >+8.1f}% | {: >8d} | {: >8s} | {}",
        itr_field,
        area_growth / initial_design_area_ * 1e2,
        resize_count_,
        delayAsString(wns, sta_, 3),
        worst_vertex != nullptr ? worst_vertex->name(network_) : "");
  }

  if (end) {
    logger_->report("---------------------------------------------------");
  }
}

}  // namespace rsz
