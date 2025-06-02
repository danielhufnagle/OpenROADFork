# GCD Complete Physical Design Flow with Held Algorithm
# Simplified version with minimal dependencies

puts "\n================================================="
puts "GCD Complete Physical Design Flow with Held Algorithm"
puts "=================================================\n"

# Error handling
proc safe_exec {cmd} {
    puts "Executing: $cmd"
    if {[catch {eval $cmd} result]} {
        puts "ERROR: $result"
        error "Failed to execute: $cmd"
    } else {
        puts "SUCCESS: $cmd"
        return $result
    }
}

# Function to get design area in um^2
proc get_design_area_um2 {} {
    return [expr [rsz::design_area] * 1e12]
}

# Load helpers
puts "Loading helpers..."
safe_exec "source test/helpers.tcl"

# Held algorithm parameters
set held_gamma 0.35
set held_max_change 0.08
set held_iterations 25

puts "\n=== Step 1: Loading Technology and Design ==="
safe_exec "read_lef test/Nangate45/Nangate45_tech.lef"
safe_exec "read_lef test/Nangate45/Nangate45_stdcell.lef"

# Define corners before reading liberty
safe_exec "define_corners default"
safe_exec "read_liberty -corner default test/Nangate45/Nangate45_typ.lib"

# Read design
safe_exec "read_verilog test/gcd_nangate45.v"
safe_exec "link_design gcd"

# Read constraints
safe_exec "read_sdc test/gcd_nangate45.sdc"

puts "\n=== Step 2: Floorplanning ==="
safe_exec "initialize_floorplan -die_area {0 0 100.13 100.8} -core_area {10.07 11.2 90.25 91} -site FreePDK45_38x28_10R_NP_162NW_34O"

# Define routing tracks
safe_exec "make_tracks metal1 -x_offset 0.19 -x_pitch 0.38 -y_offset 0.19 -y_pitch 0.38"
safe_exec "make_tracks metal2 -x_offset 0.095 -x_pitch 0.19 -y_offset 0.19 -y_pitch 0.38"
safe_exec "make_tracks metal3 -x_offset 0.19 -x_pitch 0.38 -y_offset 0.095 -y_pitch 0.19"
safe_exec "make_tracks metal4 -x_offset 0.095 -x_pitch 0.19 -y_offset 0.19 -y_pitch 0.38"
safe_exec "make_tracks metal5 -x_offset 0.19 -x_pitch 0.38 -y_offset 0.095 -y_pitch 0.19"
safe_exec "make_tracks metal6 -x_offset 0.095 -x_pitch 0.19 -y_offset 0.19 -y_pitch 0.38"
safe_exec "make_tracks metal7 -x_offset 0.19 -x_pitch 0.38 -y_offset 0.095 -y_pitch 0.19"
safe_exec "make_tracks metal8 -x_offset 0.095 -x_pitch 0.19 -y_offset 0.19 -y_pitch 0.38"
safe_exec "make_tracks metal9 -x_offset 0.19 -x_pitch 0.38 -y_offset 0.095 -y_pitch 0.19"
safe_exec "make_tracks metal10 -x_offset 0.095 -x_pitch 0.19 -y_offset 0.19 -y_pitch 0.38"

puts "\n=== Step 3: I/O Placement ==="
safe_exec "place_pins -hor_layers metal7 -ver_layers metal6"

puts "\n=== Step 4: Tapcell Insertion ==="
safe_exec "tapcell -distance 14 -tapcell_master TAPCELL_X1 -endcap_master TAPCELL_X1"

puts "\n=== Step 5: Power Distribution Network ==="
# Simple PDN for Nangate45
safe_exec "pdngen -reset"
safe_exec "add_global_connection -net VDD -pin_pattern VDD -power"
safe_exec "add_global_connection -net VSS -pin_pattern VSS -ground"
safe_exec "set_voltage_domain -power VDD -ground VSS"
safe_exec "define_pdn_grid -name main_grid -pins metal1"
safe_exec "add_pdn_stripe -layer metal1 -width 0.48 -pitch 5.0 -offset 0.0 -followpins"
safe_exec "add_pdn_stripe -layer metal2 -width 0.48 -pitch 56.0 -offset 2.0 -starts_with POWER"
safe_exec "add_pdn_stripe -layer metal4 -width 0.48 -pitch 56.0 -offset 2.0 -starts_with POWER"
safe_exec "add_pdn_stripe -layer metal7 -width 1.40 -pitch 40.0 -offset 2.0 -starts_with POWER"
safe_exec "add_pdn_connect -layers {metal1 metal2}"
safe_exec "add_pdn_connect -layers {metal2 metal4}"
safe_exec "add_pdn_connect -layers {metal4 metal7}"
safe_exec "pdngen"

puts "\n=== Step 6: Global Placement ==="
# Set routing layers
safe_exec "set_routing_layers -signal metal2-metal10 -clock metal6-metal10"
safe_exec "set_macro_extension 2"

# Set wire RC before placement
puts "Setting wire RC..."
safe_exec "set_wire_rc -corner default -layer metal3"

# Run global placement
safe_exec "global_placement -routability_driven -density 0.6"

# Re-place pins after global placement
safe_exec "place_pins -hor_layers metal7 -ver_layers metal6"

# Estimate parasitics
safe_exec "estimate_parasitics -placement"

puts "\n=== Step 7: Get Baseline Metrics ==="
set baseline_wns [worst_slack -max]
set baseline_tns [total_negative_slack -max]
set baseline_area [get_design_area_um2]

puts "Baseline metrics before optimization:"
puts "  WNS: [format %.3f $baseline_wns] ns"
puts "  TNS: [format %.3f $baseline_tns] ns"
puts "  Area: [format %.1f $baseline_area] um²"

puts "\n=== Step 8: Repair Design Violations ==="
# Repair slew/cap/fanout violations
safe_exec "set_dont_use {*CLKGT* *S_* *ISO* *_DEC* *_LH* LOGIC0_X1 LOGIC1_X1}"
safe_exec "repair_design -slew_margin 0.1 -cap_margin 0.1"

# Repair tie fanout
safe_exec "repair_tie_fanout -separation 3 LOGIC0_X1/Z"
safe_exec "repair_tie_fanout -separation 3 LOGIC1_X1/Z"

puts "\n=== Step 9: Apply Held Gate Sizing Algorithm ==="
# Save checkpoint
safe_exec "write_db held_checkpoint.db"

# Get clock period
set clocks [all_clocks]
if {[llength $clocks] > 0} {
    set clock_period [get_property [lindex $clocks 0] period]
    puts "Clock period: $clock_period ns"
} else {
    set clock_period 0.4
    puts "No clock found, using default period: $clock_period ns"
}

# Apply Held optimization
puts "\nApplying Held gate sizing with parameters:"
puts "  γ (gamma): $held_gamma"
puts "  Max change: $held_max_change"
puts "  Iterations: $held_iterations"

safe_exec "::rsz::set_held_sizing_parameters_cmd $held_gamma $held_max_change $held_iterations"

set held_start_time [clock seconds]
set clock_period_seconds [expr $clock_period * 1e-9]
set held_result [safe_exec "::rsz::optimize_gate_sizing_held_cmd $clock_period_seconds $held_gamma $held_max_change $held_iterations"]
set held_runtime [expr [clock seconds] - $held_start_time]

# Re-estimate parasitics
safe_exec "estimate_parasitics -placement"

# Get metrics after Held
set held_wns [worst_slack -max]
set held_tns [total_negative_slack -max]
set held_area [get_design_area_um2]

puts "\nHeld optimization results:"
puts "  Runtime: ${held_runtime}s"
puts "  WNS: [format %.3f $held_wns] ns (improvement: [format %+.3f [expr $held_wns - $baseline_wns]] ns)"
puts "  TNS: [format %.3f $held_tns] ns (improvement: [format %+.3f [expr $held_tns - $baseline_tns]] ns)"
puts "  Area: [format %.1f $held_area] um² (change: [format %+.1f [expr ($held_area - $baseline_area) / $baseline_area * 100]]%)"

# Cleanup checkpoint
file delete -force held_checkpoint.db

puts "\n=== Step 10: Detailed Placement ==="
safe_exec "set_placement_padding -global -left 1 -right 1"
safe_exec "detailed_placement"

puts "\n=== Step 11: Clock Tree Synthesis ==="
# Repair clock inverters
safe_exec "repair_clock_inverters"

# Run CTS
safe_exec "clock_tree_synthesis -root_buf BUF_X4 -buf_list BUF_X4 -sink_clustering_enable -sink_clustering_max_diameter 50"

# Repair clock nets
safe_exec "repair_clock_nets"

# Re-run detailed placement
safe_exec "detailed_placement"

puts "\n=== Step 12: Post-CTS Timing Repair ==="
safe_exec "set_propagated_clock [all_clocks]"
safe_exec "estimate_parasitics -placement"
safe_exec "repair_timing -skip_gate_cloning"
safe_exec "detailed_placement"

# Report post-CTS timing
puts "\nPost-CTS timing:"
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3

puts "\n=== Step 13: Global Routing ==="
safe_exec "set_global_routing_layer_adjustment metal1 0.8"
safe_exec "set_global_routing_layer_adjustment metal2 0.8"
safe_exec "set_global_routing_layer_adjustment metal3 0.8"
safe_exec "set_global_routing_layer_adjustment metal4 0.8"

safe_exec "global_route -guide_file gcd_held.guide -congestion_iterations 50"

puts "\n=== Step 14: Antenna Repair ==="
safe_exec "repair_antennas -iterations 3"
safe_exec "check_antennas"

puts "\n=== Step 15: Detailed Routing ==="
safe_exec "detailed_route -bottom_routing_layer metal2 -top_routing_layer metal10 -via_in_pin_bottom_layer metal2 -via_in_pin_top_layer metal10"

set drv_count [detailed_route_num_drvs]
puts "DRC violations after detailed routing: $drv_count"

puts "\n=== Step 16: Filler Cell Placement ==="
safe_exec "filler_placement FILL*"
safe_exec "check_placement -verbose"

puts "\n=== Step 17: Final Parasitics and Timing ==="
# Final parasitics estimation
safe_exec "estimate_parasitics -global_routing"

puts "\n=== Final Reports ==="
puts "\n--- Timing Summary ---"
report_worst_slack -min -digits 3
report_worst_slack -max -digits 3
report_tns -digits 3
report_clock_skew -digits 3

puts "\n--- Design Statistics ---"
# Custom area report with correct calculation
set final_area_um2 [get_design_area_um2]
set final_util [format %.1f [expr [rsz::utilization] * 100]]
puts "Design area [format %.0f $final_area_um2] um² ${final_util}% utilization."

# Get final metrics
set final_wns_min [sta::worst_slack -min]
set final_wns_max [sta::worst_slack -max]
set final_tns [sta::total_negative_slack -max]

puts "\n================================================="
puts "Flow Summary with Held Algorithm"
puts "================================================="
puts "Initial metrics (after global placement):"
puts "  WNS: [format %.3f $baseline_wns] ns"
puts "  TNS: [format %.3f $baseline_tns] ns"
puts "  Area: [format %.1f $baseline_area] um²"
puts "\nFinal metrics (after complete flow):"
puts "  WNS (setup): [format %.3f $final_wns_max] ns"
puts "  WNS (hold): [format %.3f $final_wns_min] ns"
puts "  TNS: [format %.3f $final_tns] ns"
puts "  Area: [format %.1f $final_area_um2] um²"
puts "  DRC violations: $drv_count"

# Save final outputs
puts "\n=== Saving Results ==="
safe_exec "write_db gcd_held_final.db"
safe_exec "write_def gcd_held_final.def"
safe_exec "write_verilog gcd_held_final.v"

puts "\n=== Physical Design Flow with Held Algorithm Complete ==="
puts "Results saved:"
puts "  Database: gcd_held_final.db"
puts "  DEF: gcd_held_final.def"
puts "  Verilog: gcd_held_final.v"
puts "  Routing guide: gcd_held.guide" 