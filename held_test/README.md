# Held Algorithm Test Flow

This directory contains a complete physical design flow using the Held gate sizing algorithm.

## Files

- `gcd_nangate45_held.tcl` - Main configuration file that sets up the design
- `held_config.tcl` - Configuration for result directory override
- `held_flow.tcl` - Complete physical design flow with Held algorithm integration
- `run_held_flow.sh` - Script to run the flow
- `results/` - Output directory containing all generated files

## Running the Flow

```bash
# From the held_test directory:
bash run_held_flow.sh

# Or directly:
../build/src/openroad -no_splash gcd_nangate45_held.tcl
```

## Configuration

The flow can be customized by modifying the Held algorithm parameters in `gcd_nangate45_held.tcl`:

```tcl
# Held algorithm parameters
set held_gamma 0.35        # Timing weight factor
set held_max_change 0.08   # Maximum size change per iteration
set held_iterations 25     # Maximum iterations
```

## Results

After running, check:
- `held_flow_debug.log` - Complete log of the flow
- `flow_summary.md` - Summary of results
- `results/` - All output files including:
  - Final Verilog netlist
  - DEF with routing
  - SPEF parasitic data
  - Timing reports

## Notes

- The flow uses the GCD design from the OpenROAD test suite
- Nangate45 technology library is required
- The Held algorithm replaces the default gate sizing step
- All other flow steps remain standard OpenROAD commands 