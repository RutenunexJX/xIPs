package tb_reset_package;
  timeunit 1ps;
  timeprecision 1ps;
  import tb_signal_package::*;

  class reset_driver #(type T = logic);
    static task run(input signal_driver#(T) target, input longint unsigned cycles,
                    input T asserted_value, input T released_value, input bit rising,
                    input cancellation cancel, output status_t status);
      status = TB_OK;
      if (target == null || $bits(T) != 1 || $isunknown({asserted_value, released_value})) begin
        status = TB_INVALID_ARGUMENT; return;
      end
      if (cancel != null && cancel.requested) begin status = TB_CANCELLED; return; end
      if (cycles == 0) return;
      target.drive(asserted_value);
      fork
        begin
          fork
            begin
              repeat (cycles) target.wait_edge(rising);
              if (cancel == null || !cancel.requested) target.drive(released_value);
            end
            begin
              if (cancel == null) wait (0);
              else wait (cancel.requested);
            end
          join_any
          disable fork;
        end
      join
      if (cancel != null && cancel.requested) status = TB_CANCELLED;
    endtask
  endclass
endpackage
