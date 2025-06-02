# Held Algorithm Production Template
# Template for integrating Held gate sizing into OpenROAD flows

# This template assumes you have already:
# 1. Loaded technology files (LEF)
# 2. Read design (Verilog/DEF)
# 3. Completed floorplanning and placement

proc apply_held_optimization {args} {
    # Default parameters based on research
    set gamma 0.35
    set max_change 0.08  
    set iterations 25
    set clock_period ""
    
    # Parse arguments
    foreach {arg value} $args {
        switch -- $arg {
            -gamma { set gamma $value }
            -max_change { set max_change $value }
            -iterations { set iterations $value }
            -clock_period { set clock_period $value }
            default { puts "Warning: Unknown argument $arg" }
        }
    }
    
    # Get clock period if not specified
    if {$clock_period == ""} {
        set clocks [all_clocks]
        if {[llength $clocks] > 0} {
            set clock_period [get_property [lindex $clocks 0] period]
            puts "Using clock period from design: $clock_period"
        } else {
            error "No clock found and -clock_period not specified"
        }
    }
    
    puts "\n=== Applying Held Gate Sizing Optimization ==="
    puts "Parameters:"
    puts "  γ (gamma): $gamma"
    puts "  Max change: $max_change"
    puts "  Iterations: $iterations"
    puts "  Clock period: $clock_period"
    
    # Save current state
    set checkpoint_file "held_optimization_checkpoint.db"
    write_db $checkpoint_file
    
    # Get initial metrics
    set initial_wns [worst_slack -max]
    set initial_tns [total_negative_slack -max]
    set initial_area [rsz::design_area]
    
    puts "\nInitial metrics:"
    puts "  WNS: [format %.3f $initial_wns] ns"
    puts "  TNS: [format %.3f $initial_tns] ns"
    puts "  Area: [format %.1f $initial_area] um²"
    
    # Apply Held optimization
    ::rsz::set_held_sizing_parameters_cmd $gamma $max_change $iterations
    
    set start_time [clock seconds]
    set success [::rsz::optimize_gate_sizing_held_cmd \
        [sta::time_ui_sta $clock_period] $gamma $max_change $iterations]
    set runtime [expr [clock seconds] - $start_time]
    
    # Get final metrics
    set final_wns [worst_slack -max]
    set final_tns [total_negative_slack -max]
    set final_area [rsz::design_area]
    
    # Calculate improvements
    set wns_improvement [expr $final_wns - $initial_wns]
    set tns_improvement [expr $final_tns - $initial_tns]
    set area_increase [expr ($final_area - $initial_area) / $initial_area * 100]
    
    puts "\nOptimization Results:"
    puts "  Success: $success"
    puts "  Runtime: ${runtime}s"
    puts "  WNS improvement: [format %+.3f $wns_improvement] ns"
    puts "  TNS improvement: [format %+.3f $tns_improvement] ns"
    puts "  Area increase: [format %+.1f $area_increase]%"
    
    # Decide whether to keep changes
    if {$wns_improvement >= 0 && $area_increase < 10} {
        puts "\nOptimization accepted"
        file delete -force $checkpoint_file
    } else {
        puts "\nOptimization rejected - restoring original state"
        clear
        read_db $checkpoint_file
        file delete -force $checkpoint_file
    }
    
    return $success
}

# Example usage in production flow:
#
# After global placement:
# global_placement -timing_driven
#
# Setup parasitics (choose one method):
# Method 1: Placement-based
# set_wire_rc -corner default -resistance 1.0e-4 -capacitance 1.0e-5
# estimate_parasitics -placement
#
# Method 2: Global routing-based
# global_route
# estimate_parasitics -global_routing
#
# Apply Held optimization
# apply_held_optimization -gamma 0.35 -max_change 0.08 -iterations 25
#
# Continue with detailed placement
# detailed_placement 