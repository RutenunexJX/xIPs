`include "reset_config.svh"

module reset_gen #(
    parameter int unsigned STAGES = `XIPS_RESET_STAGES
) (
    input  logic clk_i,
    input  logic async_rst_ni,
    output logic sync_rst_o
);
    logic [STAGES-1:0] synchronizer_q;

    always_ff @(posedge clk_i or negedge async_rst_ni) begin
        if (!async_rst_ni)
            synchronizer_q <= '0;
        else
            synchronizer_q <= {synchronizer_q[STAGES-2:0], 1'b1};
    end

    assign sync_rst_o = !synchronizer_q[STAGES-1];
endmodule
