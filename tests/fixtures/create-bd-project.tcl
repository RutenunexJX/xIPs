# Run with Vivado batch mode from a fresh, dedicated fixture directory.
if {[catch {
    set root [file normalize ./fixture]
    if {[file exists $root]} {error "Fixture directory already exists"}
    create_project fixture $root -part xc7a35tcpg236-1
    create_bd_design design_1
    create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant:1.1 constant_0
    create_bd_port -dir O dout
    connect_bd_net [get_bd_pins constant_0/dout] [get_bd_ports dout]
    validate_bd_design
    save_bd_design
    generate_target all [get_files design_1.bd]
    set wrappers [make_wrapper -files [get_files design_1.bd] -top]
    add_files -norecurse $wrappers
    set_property top design_1_wrapper [current_fileset]
    close_project
} reason]} {
    puts stderr "FIXTURE_ERROR: $reason"
    exit 1
}
exit 0
