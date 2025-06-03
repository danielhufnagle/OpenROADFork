# Configuration for Held flow test
# This file sets up the correct paths and result directory

# Override result_dir for our held test
proc make_result_file { filename } {
  set result_dir "../held_test/results"
  if { ![file exists $result_dir] } {
    file mkdir $result_dir
  }
  set root [file rootname $filename]
  set ext [file extension $filename]
  set filename "$root-tcl$ext"
  return [file join $result_dir $filename]
}

# Held algorithm parameters
set held_gamma 0.35
set held_max_change 0.08
set held_iterations 25 