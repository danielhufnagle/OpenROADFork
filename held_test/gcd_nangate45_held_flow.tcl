# GCD Complete Physical Design Flow with Held Algorithm
# This script performs a complete physical design flow using the Held gate sizing algorithm
# instead of OpenROAD's default gate sizing

puts "\n================================================="
puts "GCD Physical Design Flow with Held Algorithm"
puts "=================================================\n"

# Load helpers
source "test/helpers.tcl"
source "test/flow_helpers.tcl"
source "test/Nangate45/Nangate45.vars"

# Design configuration
set design "gcd"
set top_module "gcd"
set synth_verilog "test/gcd_nangate45.v"
set sdc_file "test/gcd_nangate45.sdc"
set die_area {0 0 100.13 100.8}
set core_area {10.07 11.2 90.25 91}

# Held algorithm parameters
set held_gamma 0.35
set held_max_change 0.08
set held_iterations 25

# Read design
read_libraries
read_verilog $synth_verilog
link_design $top_module
read_sdc $sdc_file

set_thread_count [cpu_count]
sta::set_thread_count 1

puts "\n=== Step 1: Floorplanning ==="
initialize_floorplan -site $site \
  -die_area $die_area \
  -core_area $core_area

source $tracks_file

# Remove buffers inserted by synthesis
remove_buffers

puts "\n=== Step 2: IO Placement ==="
place_pins -random -hor_layers $io_placer_hor_layer -ver_layers $io_placer_ver_layer

puts "\n=== Step 3: Tapcell Insertion ==="
eval tapcell $tapcell_args

puts "\n=== Step 4: Power Distribution Network ==="
source $pdn_cfg
pdngen

puts "\n=== Step 5: Global Placement ==="
foreach layer_adjustment $global_routing_layer_adjustments {
  lassign $layer_adjustment layer adjustment
  set_global_routing_layer_adjustment $layer $adjustment
}
set_routing_layers -signal $global_routing_layers \
  -clock $global_routing_clock_layers
set_macro_extension 2

global_placement -routability_driven -density $global_place_density \
  -pad_left $global_place_pad -pad_right $global_place_pad

# Update IO placement
place_pins -hor_layers $io_placer_hor_layer -ver_layers $io_placer_ver_layer

# Checkpoint after global placement
set global_place_db [make_result_file ${design}_${platform}_held_global_place.db]
write_db $global_place_db

puts "\n=== Step 6: Setup for Timing Optimization ==="
source $layer_rc_file
set_wire_rc -signal -layer $wire_rc_layer
set_wire_rc -clock  -layer $wire_rc_layer_clk
set_dont_use $dont_use

# Estimate parasitics after placement
estimate_parasitics -placement

puts "\n=== Step 7: Repair Design Violations ==="
# First repair max slew/cap/fanout violations
repair_design -slew_margin $slew_margin -cap_margin $cap_margin

# Repair tie fanout
repair_tie_fanout -separation $tie_separation $tielo_port
repair_tie_fanout -separation $tie_separation $tiehi_port

puts "\n=== Step 8: Apply Held Gate Sizing Algorithm ==="
# Get baseline metrics before Held optimization
set baseline_wns [worst_slack -max]
set baseline_tns [total_negative_slack -max]
set baseline_area [rsz::design_area]

puts "Baseline metrics before Held optimization:"
puts "  WNS: [format %.3f $baseline_wns] ns"
puts "  TNS: [format %.3f $baseline_tns] ns"
puts "  Area: [format %.1f $baseline_area] um²"

# Save checkpoint before Held optimization
set held_checkpoint "held_optimization_checkpoint.db"
write_db $held_checkpoint

# Apply Held optimization
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
::rsz::set_held_sizing_parameters_cmd $held_gamma $held_max_change $held_iterations

set held_start_time [clock seconds]
set clock_period_seconds [expr $clock_period * 1e-9]
set held_success [::rsz::optimize_gate_sizing_held_cmd \
    $clock_period_seconds $held_gamma $held_max_change $held_iterations]
set held_runtime [expr [clock seconds] - $held_start_time]

# Re-estimate parasitics after Held optimization
estimate_parasitics -placement

# Get metrics after Held optimization
set held_wns [worst_slack -max]
set held_tns [total_negative_slack -max]
set held_area [rsz::design_area]

puts "\nHeld optimization results:"
puts "  Success: $held_success"
puts "  Runtime: ${held_runtime}s"
puts "  WNS: [format %.3f $held_wns] ns (improvement: [format %+.3f [expr $held_wns - $baseline_wns]] ns)"
puts "  TNS: [format %.3f $held_tns] ns (improvement: [format %+.3f [expr $held_tns - $baseline_tns]] ns)"
puts "  Area: [format %.1f $held_area] um² (increase: [format %+.1f [expr ($held_area - $baseline_area) / $baseline_area * 100]]%)"

# Delete checkpoint
file delete -force $held_checkpoint

puts "\n=== Step 9: Detailed Placement ==="
set_placement_padding -global -left $detail_place_pad -right $detail_place_pad
detailed_placement

# Post placement timing report
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
report_check_types -max_slew -max_capacitance -max_fanout -violators

puts "\n=== Step 10: Clock Tree Synthesis ==="
# Repair clock inverters
repair_clock_inverters

# Run CTS
clock_tree_synthesis -root_buf $cts_buffer -buf_list $cts_buffer \
  -sink_clustering_enable \
  -sink_clustering_max_diameter $cts_cluster_diameter

# Repair clock nets
repair_clock_nets

# Place clock buffers
detailed_placement

# Checkpoint after CTS
set cts_db [make_result_file ${design}_${platform}_held_cts.db]
write_db $cts_db

puts "\n=== Step 11: Setup/Hold Timing Repair ==="
set_propagated_clock [all_clocks]

# Estimate parasitics for timing repair
estimate_parasitics -placement

# Repair timing (without gate cloning to preserve Held optimization)
repair_timing -skip_gate_cloning

# Report timing after repair
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3

puts "\n=== Step 12: Final Detailed Placement ==="
detailed_placement

# Capture utilization before fillers
utl::metric "DPL::utilization" [format %.1f [expr [rsz::utilization] * 100]]
utl::metric "DPL::design_area" [sta::format_area [rsz::design_area] 0]

# Checkpoint after detailed placement
set dpl_db [make_result_file ${design}_${platform}_held_dpl.db]
write_db $dpl_db

puts "\n=== Step 13: Global Routing ==="
pin_access -bottom_routing_layer $min_routing_layer \
           -top_routing_layer $max_routing_layer

set route_guide [make_result_file ${design}_${platform}_held.route_guide]
global_route -guide_file $route_guide \
  -congestion_iterations 100 -verbose

puts "\n=== Step 14: Antenna Repair ==="
repair_antennas -iterations 5
check_antennas

puts "\n=== Step 15: Detailed Routing ==="
# Run pin access again
pin_access -bottom_routing_layer $min_routing_layer \
           -top_routing_layer $max_routing_layer

detailed_route -output_drc [make_result_file "${design}_${platform}_held_route_drc.rpt"] \
               -output_maze [make_result_file "${design}_${platform}_held_maze.log"] \
               -no_pin_access \
               -save_guide_updates \
               -bottom_routing_layer $min_routing_layer \
               -top_routing_layer $max_routing_layer \
               -verbose 0

set drv_count [detailed_route_num_drvs]
puts "DRC violations: $drv_count"

# Save routed design
set routed_db [make_result_file ${design}_${platform}_held_route.db]
write_db $routed_db

puts "\n=== Step 16: Filler Cell Placement ==="
filler_placement $filler_cells
check_placement -verbose

# Final checkpoint
set fill_db [make_result_file ${design}_${platform}_held_fill.db]
write_db $fill_db

puts "\n=== Step 17: Parasitic Extraction ==="
if { $rcx_rules_file != "" } {
  define_process_corner -ext_model_index 0 X
  extract_parasitics -ext_model_file $rcx_rules_file
  
  set spef_file [make_result_file ${design}_${platform}_held.spef]
  write_spef $spef_file
  read_spef $spef_file
} else {
  # Use global routing based parasitics
  estimate_parasitics -global_routing
}

puts "\n=== Final Reports ==="
puts "\n--- Timing Reports ---"
report_checks -path_delay min_max -format full_clock_expanded \
  -fields {input_pin slew capacitance} -digits 3
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
report_check_types -max_slew -max_capacitance -max_fanout -violators -digits 3
report_clock_skew -digits 3

puts "\n--- Power Report ---"
report_power -corner $power_corner

puts "\n--- Design Statistics ---"
report_design_area
report_floating_nets -verbose

# Final metrics
set final_wns_min [sta::worst_slack -min]
set final_wns_max [sta::worst_slack -max]
set final_tns [sta::total_negative_slack -max]
set final_area [rsz::design_area]

puts "\n=== Flow Summary with Held Algorithm ==="
puts "Initial metrics (after global placement):"
puts "  WNS: [format %.3f $baseline_wns] ns"
puts "  TNS: [format %.3f $baseline_tns] ns"
puts "  Area: [format %.1f $baseline_area] um²"
puts "\nFinal metrics (after complete flow):"
puts "  WNS (setup): [format %.3f $final_wns_max] ns"
puts "  WNS (hold): [format %.3f $final_wns_min] ns"
puts "  TNS: [format %.3f $final_tns] ns"
puts "  Area: [format %.1f $final_area] um²"
puts "  DRC violations: $drv_count"

# Save final DEF
set final_def [make_result_file ${design}_${platform}_held_final.def]
write_def $final_def

puts "\n=== Physical Design Flow with Held Algorithm Complete ==="
puts "Results saved with prefix: ${design}_${platform}_held_*" 