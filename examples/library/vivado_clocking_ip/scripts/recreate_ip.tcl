create_ip -name clk_wiz -vendor xilinx.com -library ip -module_name clk_wiz_0
set_property -dict [list CONFIG.PRIM_IN_FREQ {100.000}] [get_ips clk_wiz_0]
generate_target all [get_ips clk_wiz_0]
