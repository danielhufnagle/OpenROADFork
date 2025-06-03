# gcd flow with Held algorithm
source "test/helpers.tcl"
source "test/flow_helpers.tcl"

# Change to test directory for correct relative paths
cd test

# Now source Nangate45 vars from current directory
source "Nangate45/Nangate45.vars"

# Source held config to set basic config
source "../held_test/held_config.tcl"

set design "gcd"
set top_module "gcd"
set synth_verilog "gcd_nangate45.v"
set sdc_file "gcd_nangate45.sdc"
set die_area {0 0 100.13 100.8}
set core_area {10.07 11.2 90.25 91}

# Set result directory to held_test
set result_dir "../held_test/results"

# Held algorithm parameters - OVERRIDE config file settings
set held_gamma 0.2
set held_max_change 0.03
set held_iterations 10

puts "Using Held parameters: gamma=$held_gamma, max_change=$held_max_change, iterations=$held_iterations"

# Source the flow from held_test directory
source "../held_test/held_flow.tcl" 