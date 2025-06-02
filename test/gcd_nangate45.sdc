create_clock [get_ports clk] -name core_clock -period 0.4850

# Set input delays (assume 30% of clock period for inputs)
set input_delay [expr 0.4850 * 0.3]
set_input_delay $input_delay -clock core_clock [all_inputs]

# Set output delays (assume 30% of clock period for outputs)  
set output_delay [expr 0.4850 * 0.3]
set_output_delay $output_delay -clock core_clock [all_outputs]

# Set clock uncertainty (assume 10% of period)
set_clock_uncertainty [expr 0.4850 * 0.1] core_clock
