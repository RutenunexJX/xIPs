package tb_sequence_package;
  timeunit 1ps;
  timeprecision 1ps;
  import tb_signal_package::*;

  class ramp_driver #(type T = logic [7:0]);
    static task run(input signal_driver#(T) target, input longint unsigned count,
                    input T start_value, input T step, input bit rising,
                    input cancellation cancel, output status_t status);
      T value;
      status = TB_OK;
      if (target == null || $isunknown({start_value, step})) begin status = TB_INVALID_ARGUMENT; return; end
      if (cancel != null && cancel.requested) begin status = TB_CANCELLED; return; end
      if (count == 0) return;
      value = start_value;
      fork
        begin
          fork
            begin
              repeat (count) begin
                target.wait_edge(rising);
                if (cancel != null && cancel.requested) break;
                target.drive(value);
                value = T'(value + step);
              end
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
