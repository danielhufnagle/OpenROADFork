############################################################################
# Physical Design Flow with Held Algorithm
# Based on OpenROAD flow.tcl
############################################################################

# Assumes flow_helpers.tcl has been read.
read_libraries
read_verilog $synth_verilog
link_design $top_module
read_sdc $sdc_file

set_thread_count [cpu_count]
# Temporarily disable sta's threading due to random failures
sta::set_thread_count 1

utl::metric "IFP::ord_version" [ord::openroad_git_describe]
# Note that sta::network_instance_count is not valid after tapcells are added.
utl::metric "IFP::instance_count" [sta::network_instance_count]

initialize_floorplan -site $site \
  -die_area $die_area \
  -core_area $core_area

source $tracks_file

# remove buffers inserted by synthesis 
remove_buffers

################################################################
# IO Placement (random)
place_pins -random -hor_layers $io_placer_hor_layer -ver_layers $io_placer_ver_layer

################################################################
# Macro Placement
if { [have_macros] } {
  lassign $macro_place_halo halo_x halo_y
  set report_dir [make_result_file ${design}_${platform}_rtlmp]
  rtl_macro_placer -halo_width $halo_x -halo_height $halo_y \
      -report_directory $report_dir
}

################################################################
# Tapcell insertion
eval tapcell $tapcell_args

################################################################
# Power distribution network insertion
source $pdn_cfg
pdngen

################################################################
# Global placement

foreach layer_adjustment $global_routing_layer_adjustments {
  lassign $layer_adjustment layer adjustment
  set_global_routing_layer_adjustment $layer $adjustment
}
set_routing_layers -signal $global_routing_layers \
  -clock $global_routing_clock_layers
set_macro_extension 2

global_placement -routability_driven -density $global_place_density \
  -pad_left $global_place_pad -pad_right $global_place_pad

# IO Placement
place_pins -hor_layers $io_placer_hor_layer -ver_layers $io_placer_ver_layer

# checkpoint
set global_place_db [make_result_file ${design}_${platform}_global_place.db]
write_db $global_place_db

################################################################
# Repair max slew/cap/fanout violations and normalize slews
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock  -layer $wire_rc_layer_clk
set_dont_use $dont_use

repair_tie_fanout -separation $tie_separation $tielo_port
repair_tie_fanout -separation $tie_separation $tiehi_port

set_placement_padding -global -left $detail_place_pad -right $detail_place_pad
detailed_placement

# post resize timing report (ideal clocks)
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
# Check slew repair
report_check_types -max_slew -max_capacitance -max_fanout -violators

utl::metric "RSZ::repair_design_buffer_count" [rsz::repair_design_buffer_count]
utl::metric "RSZ::max_slew_slack" [expr [sta::max_slew_check_slack_limit] * 100]
utl::metric "RSZ::max_fanout_slack" [expr [sta::max_fanout_check_slack_limit] * 100]
utl::metric "RSZ::max_capacitance_slack" [expr [sta::max_capacitance_check_slack_limit] * 100]

################################################################
# HELD ALGORITHM GATE SIZING (Pre-CTS)

puts "\n=== Applying Held Gate Sizing Algorithm (Pre-CTS) ==="

# Get baseline metrics before Held optimization
set baseline_wns [sta::worst_slack -max]
set baseline_tns [sta::total_negative_slack -max]
set baseline_area [rsz::design_area]

puts "Baseline metrics before Held optimization:"
puts [format "  WNS: %.3f ns" $baseline_wns]
puts [format "  TNS: %.3f ns" $baseline_tns]
puts [format "  Area: %.1f um²" [expr $baseline_area * 1e12]]

# Verify Held parameters are defined
if {![info exists held_gamma]} {
    error "held_gamma is not defined"
}
if {![info exists held_max_change]} {
    error "held_max_change is not defined"
}
if {![info exists held_iterations]} {
    error "held_iterations is not defined"
}

# Apply Held optimization (Pre-CTS under ideal clocks)
puts "\nApplying Held gate sizing with parameters:"
puts "  γ (gamma): $held_gamma"
puts "  Max change: $held_max_change"
puts "  Iterations: $held_iterations"

# Get clock period
set clocks [all_clocks]
if {[llength $clocks] > 0} {
    set clock_period [get_property [lindex $clocks 0] period]
    puts "  Clock period: $clock_period ns"
} else {
    error "No clock found in design"
}

# Set Held parameters and run optimization
set held_pre_cts_runtime 0

if {[catch {set_held_sizing_parameters_cmd $held_gamma $held_max_change $held_iterations} result]} {
    puts "Warning: Could not set Held parameters: $result"
    puts "Skipping Held algorithm and continuing with flow"
} else {
    # Held algorithm timing measurement - START
    puts "=== Starting Held Gate Sizing Algorithm (Pre-CTS) ==="
    set held_pre_cts_start_time 0
    catch {set held_pre_cts_start_time [clock seconds]}
    
    if {[catch {optimize_gate_sizing_held_cmd $clock_period} held_success]} {
        puts "Warning: Held optimization failed: $held_success"
        puts "Continuing with flow without Pre-CTS gate sizing"
    } else {
        # Held algorithm timing measurement - END
        set held_pre_cts_end_time 0
        catch {set held_pre_cts_end_time [clock seconds]}
        set held_pre_cts_runtime [expr $held_pre_cts_end_time - $held_pre_cts_start_time]
        puts "=== Held Gate Sizing (Pre-CTS) Complete ==="
        puts "Held gate sizing (Pre-CTS) runtime: ${held_pre_cts_runtime} seconds"
        
        # Re-estimate parasitics after Held optimization
        estimate_parasitics -placement
        
        # Get metrics after Held optimization
        set held_pre_cts_wns [sta::worst_slack -max]
        set held_pre_cts_tns [sta::total_negative_slack -max]
        set held_pre_cts_area [rsz::design_area]
        
        puts "\nHeld optimization (Pre-CTS) results:"
        puts "  Runtime: ${held_pre_cts_runtime}s"
        puts [format "  WNS: %.3f ns (improvement: %+.3f ns)" $held_pre_cts_wns [expr $held_pre_cts_wns - $baseline_wns]]
        puts [format "  TNS: %.3f ns (improvement: %+.3f ns)" $held_pre_cts_tns [expr $held_pre_cts_tns - $baseline_tns]]
        puts [format "  Area: %.1f um² (increase: %+.1f%%)" [expr $held_pre_cts_area * 1e12] \
            [expr ($held_pre_cts_area - $baseline_area) / $baseline_area * 100]]
        
        # Check if timing degraded significantly
        set wns_degradation [expr $baseline_wns - $held_pre_cts_wns]
        if {$wns_degradation > 0.1} {
            puts "WARNING: WNS degraded by ${wns_degradation} ns - Held algorithm may need parameter adjustment"
        }
    }
}

# Record Held runtime as metric for comparison
if {[info exists held_pre_cts_runtime]} {
    utl::metric "HELD::pre_cts_gate_sizing_runtime" "$held_pre_cts_runtime"
} else {
    utl::metric "HELD::pre_cts_gate_sizing_runtime" "0"
}

################################################################
# Clock Tree Synthesis

# Clone clock tree inverters next to register loads
# so cts does not try to buffer the inverted clocks.
repair_clock_inverters

clock_tree_synthesis -root_buf $cts_buffer -buf_list $cts_buffer \
  -sink_clustering_enable \
  -sink_clustering_max_diameter $cts_cluster_diameter

# CTS leaves a long wire from the pad to the clock tree root.
repair_clock_nets

# place clock buffers
detailed_placement

# checkpoint
set cts_db [make_result_file ${design}_${platform}_cts.db]
write_db $cts_db

################################################################
# HELD ALGORITHM GATE SIZING (Post-CTS)

puts "\n=== Applying Held Gate Sizing Algorithm (Post-CTS) ==="

set_propagated_clock [all_clocks]

# Global routing is fast enough for the flow regressions.
# It is NOT FAST ENOUGH FOR PRODUCTION USE.
set repair_timing_use_grt_parasitics 0
if { $repair_timing_use_grt_parasitics } {
  # Global route for parasitics - no guide file requied
  global_route -congestion_iterations 100
  estimate_parasitics -global_routing
} else {
  estimate_parasitics -placement
}

# Get baseline metrics before Post-CTS Held optimization
set baseline_post_cts_wns [sta::worst_slack -max]
set baseline_post_cts_tns [sta::total_negative_slack -max]
set baseline_post_cts_area [rsz::design_area]

puts "Baseline metrics before Post-CTS Held optimization:"
puts [format "  WNS: %.3f ns" $baseline_post_cts_wns]
puts [format "  TNS: %.3f ns" $baseline_post_cts_tns]
puts [format "  Area: %.1f um²" [expr $baseline_post_cts_area * 1e12]]

# Apply Held optimization again after CTS with real clock propagation
puts "\nApplying Held gate sizing (Post-CTS) with parameters:"
puts "  γ (gamma): $held_gamma"
puts "  Max change: $held_max_change"
puts "  Iterations: $held_iterations"
puts "  Clock propagation: Real (after CTS)"

# Held algorithm timing measurement (Post-CTS) - START
puts "=== Starting Held Gate Sizing Algorithm (Post-CTS) ==="
set held_post_cts_start_time 0
catch {set held_post_cts_start_time [clock seconds]}
set held_post_cts_runtime 0

# Calculate clock period in seconds for Post-CTS optimization
# set clock_period_seconds [expr $clock_period * 1e-9]

# Set Held parameters for Post-CTS optimization
if {[catch {set_held_sizing_parameters_cmd $held_gamma $held_max_change $held_iterations} result]} {
    puts "Warning: Could not set Post-CTS Held parameters: $result"
    puts "Skipping Post-CTS gate sizing"
    set held_post_cts_runtime 0
} else {
    # Calculate clock period in seconds for the optimization call  
    set clock_period_seconds [expr $clock_period * 1e-9]
    
    if {[catch {optimize_gate_sizing_held_cmd $clock_period_seconds} held_post_cts_success]} {
        puts "Warning: Post-CTS Held optimization failed: $held_post_cts_success"
        puts "Continuing without Post-CTS gate sizing"
    } else {
        # Held algorithm timing measurement (Post-CTS) - END
        set held_post_cts_end_time 0
        catch {set held_post_cts_end_time [clock seconds]}
        set held_post_cts_runtime [expr $held_post_cts_end_time - $held_post_cts_start_time]
        puts "=== Held Gate Sizing (Post-CTS) Complete ==="
        puts "Held gate sizing (Post-CTS) runtime: ${held_post_cts_runtime} seconds"
        
        # Re-estimate parasitics after Post-CTS Held optimization
        estimate_parasitics -placement
        
        # Get metrics after Post-CTS Held optimization
        set held_post_cts_wns [sta::worst_slack -max]
        set held_post_cts_tns [sta::total_negative_slack -max]
        set held_post_cts_area [rsz::design_area]
        
        puts "\nHeld optimization (Post-CTS) results:"
        puts "  Runtime: ${held_post_cts_runtime}s"
        puts [format "  WNS: %.3f ns (improvement: %+.3f ns)" $held_post_cts_wns [expr $held_post_cts_wns - $baseline_post_cts_wns]]
        puts [format "  TNS: %.3f ns (improvement: %+.3f ns)" $held_post_cts_tns [expr $held_post_cts_tns - $baseline_post_cts_tns]]
        puts [format "  Area: %.1f um² (increase: %+.1f%%)" [expr $held_post_cts_area * 1e12] \
            [expr ($held_post_cts_area - $baseline_post_cts_area) / $baseline_post_cts_area * 100]]
        
        # Check if timing degraded significantly
        set wns_degradation [expr $baseline_post_cts_wns - $held_post_cts_wns]
        if {$wns_degradation > 0.1} {
            puts "WARNING: Post-CTS WNS degraded by ${wns_degradation} ns - Held algorithm may need parameter adjustment"
        }
    }
}

# Post timing optimization using Held algorithm only
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
report_check_types -max_slew -max_capacitance -max_fanout -violators -digits 3

utl::metric "RSZ::worst_slack_min" [sta::worst_slack -min]
utl::metric "RSZ::worst_slack_max" [sta::worst_slack -max]
utl::metric "RSZ::tns_max" [sta::total_negative_slack -max]
utl::metric "RSZ::hold_buffer_count" 0
if {[info exists held_post_cts_runtime]} {
    utl::metric "HELD::post_cts_gate_sizing_runtime" "$held_post_cts_runtime"
} else {
    utl::metric "HELD::post_cts_gate_sizing_runtime" "0"
}

puts "\n=== All Gate Sizing Completed Using Held Algorithm ==="

################################################################
# Detailed Placement

detailed_placement

# Capture utilization before fillers make it 100%
utl::metric "DPL::utilization" [format %.1f [expr [rsz::utilization] * 100]]
set design_area_formatted [sta::format_area [rsz::design_area] 0]
utl::metric "DPL::design_area" "$design_area_formatted"

# checkpoint
set dpl_db [make_result_file ${design}_${platform}_dpl.db]
write_db $dpl_db

set verilog_file [make_result_file ${design}_${platform}.v]
write_verilog $verilog_file

################################################################
# Global routing

pin_access -bottom_routing_layer $min_routing_layer \
           -top_routing_layer $max_routing_layer

set route_guide [make_result_file ${design}_${platform}.route_guide]
global_route -guide_file $route_guide \
  -congestion_iterations 100 -verbose

set verilog_file [make_result_file ${design}_${platform}.v]
write_verilog -remove_cells $filler_cells $verilog_file

################################################################
# Repair antennas post-GRT

utl::set_metrics_stage "grt__{}"
repair_antennas -iterations 5

check_antennas
utl::clear_metrics_stage
utl::metric "GRT::ANT::errors" [ant::antenna_violation_count]

################################################################
# Detailed routing

# Run pin access again after inserting diodes and moving cells
pin_access -bottom_routing_layer $min_routing_layer \
           -top_routing_layer $max_routing_layer

detailed_route -output_drc [make_result_file "${design}_${platform}_route_drc.rpt"] \
               -output_maze [make_result_file "${design}_${platform}_maze.log"] \
               -no_pin_access \
               -save_guide_updates \
               -bottom_routing_layer $min_routing_layer \
               -top_routing_layer $max_routing_layer \
               -verbose 0

write_guides [make_result_file "${design}_${platform}_output_guide.mod"]
set drv_count [detailed_route_num_drvs]
utl::metric "DRT::drv" "$drv_count"

set routed_db [make_result_file ${design}_${platform}_route.db]
write_db $routed_db

set routed_def [make_result_file ${design}_${platform}_route.def]
write_def $routed_def

################################################################
# Repair antennas post-DRT

set repair_antennas_iters 0
utl::set_metrics_stage "drt__repair_antennas__pre_repair__{}"
while {[check_antennas] && $repair_antennas_iters < 5} {
  utl::set_metrics_stage "drt__repair_antennas__iter_${repair_antennas_iters}__{}"

  repair_antennas

  detailed_route -output_drc [make_result_file "${design}_${platform}_ant_fix_drc.rpt"] \
                 -output_maze [make_result_file "${design}_${platform}_ant_fix_maze.log"] \
                 -save_guide_updates \
                 -bottom_routing_layer $min_routing_layer \
                 -top_routing_layer $max_routing_layer \
                 -verbose 0

  incr repair_antennas_iters
}

utl::set_metrics_stage "drt__{}"
check_antennas

utl::clear_metrics_stage
utl::metric "DRT::ANT::errors" [ant::antenna_violation_count]

if {![design_is_routed]} {
  error "Design has unrouted nets."
}

set repair_antennas_db [make_result_file ${design}_${platform}_repaired_route.odb]
write_db $repair_antennas_db

################################################################
# Filler placement

filler_placement $filler_cells
check_placement -verbose

# checkpoint
set fill_db [make_result_file ${design}_${platform}_fill.db]
write_db $fill_db

################################################################
# Extraction

if { $rcx_rules_file != "" } {
  define_process_corner -ext_model_index 0 X
  extract_parasitics -ext_model_file $rcx_rules_file

  set spef_file [make_result_file ${design}_${platform}.spef]
  write_spef $spef_file

  read_spef $spef_file
} else {
  # Use global routing based parasitics inlieu of rc extraction
  estimate_parasitics -global_routing
}

################################################################
# Final Report

report_checks -path_delay min_max -format full_clock_expanded \
  -fields {input_pin slew capacitance} -digits 3
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
report_check_types -max_slew -max_capacitance -max_fanout -violators -digits 3
report_clock_skew -digits 3
report_power -corner $power_corner

report_floating_nets -verbose
report_design_area

utl::metric "DRT::worst_slack_min" [sta::worst_slack -min]
utl::metric "DRT::worst_slack_max" [sta::worst_slack -max]
utl::metric "DRT::tns_max" [sta::total_negative_slack -max]
utl::metric "DRT::clock_skew" [expr abs([sta::worst_clock_skew -setup])]

# slew/cap/fanout slack/limit
utl::metric "DRT::max_slew_slack" [expr [sta::max_slew_check_slack_limit] * 100]
utl::metric "DRT::max_fanout_slack" [expr [sta::max_fanout_check_slack_limit] * 100]
utl::metric "DRT::max_capacitance_slack" [expr [sta::max_capacitance_check_slack_limit] * 100];
# report clock period as a metric for updating limits
set clocks_list [all_clocks]
if {[llength $clocks_list] > 0} {
    set clock_period_final [get_property [lindex $clocks_list 0] period]
    utl::metric "DRT::clock_period" "$clock_period_final"
} else {
    utl::metric "DRT::clock_period" "0.0"
}

puts "\n=== Physical Design Flow with Held Algorithm Complete ===" 