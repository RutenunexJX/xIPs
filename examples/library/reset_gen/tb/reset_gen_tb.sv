module reset_gen_tb;
    logic clk = 0;
    logic async_rst_n = 0;
    logic sync_rst;

    always #5 clk = ~clk;

    reset_gen #(.STAGES(2)) dut (
        .clk_i(clk),
        .async_rst_ni(async_rst_n),
        .sync_rst_o(sync_rst)
    );

    initial begin
        repeat (2) @(posedge clk);
        async_rst_n = 1;
        repeat (4) @(posedge clk);
        assert (!sync_rst);
        $finish;
    end
endmodule
