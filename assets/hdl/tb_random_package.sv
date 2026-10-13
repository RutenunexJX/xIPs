package tb_random_package;
  timeunit 1ps;
  timeprecision 1ps;
  import tb_signal_package::*;

  class bounded_value #(type T = logic [7:0]);
    rand T value;
    T minimum, maximum;
    constraint inclusive_range { value >= minimum; value <= maximum; }
  endclass

  class random_driver #(type T = logic [7:0]);
    static task run(input signal_driver#(T) target, input longint unsigned count,
                    input T minimum, input T maximum, input bit rising,
                    input cancellation cancel, output status_t status);
      bounded_value#(T) generator;
      status = TB_OK;
      if (target == null || $isunknown({minimum, maximum}) || minimum > maximum) begin
        status = TB_INVALID_ARGUMENT; return;
      end
      if (cancel != null && cancel.requested) begin status = TB_CANCELLED; return; end
      if (count == 0) return;
      generator = new; generator.minimum = minimum; generator.maximum = maximum;
      fork
        begin
          fork
            begin
              repeat (count) begin
                target.wait_edge(rising);
                if (cancel != null && cancel.requested) break;
                if (!generator.randomize()) begin status = TB_RANDOM_FAILED; break; end
                target.drive(generator.value);
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

    // Bounds are absolute simulation ticks in picoseconds, not a relative delay.
    static task window(input signal_driver#(T) target, input time start_at_ps, input time stop_at_ps,
                       input T minimum, input T maximum, input bit rising,
                       input cancellation cancel, output status_t status);
      bounded_value#(T) generator;
      status = TB_OK;
      if (target == null || $isunknown({start_at_ps, stop_at_ps, minimum, maximum}) ||
          start_at_ps >= stop_at_ps || minimum > maximum) begin
        status = TB_INVALID_ARGUMENT; return;
      end
      if (cancel != null && cancel.requested) begin status = TB_CANCELLED; return; end
      if ($time >= stop_at_ps) begin status = TB_EXPIRED; return; end
      generator = new; generator.minimum = minimum; generator.maximum = maximum;
      // The outer process owns only this invocation's workers. Unrelated caller
      // processes and other concurrent invocations cannot be disabled here.
      fork
        begin
          fork
            begin
              forever begin
                target.wait_edge(rising);
                if ($time >= stop_at_ps || (cancel != null && cancel.requested)) break;
                if ($time >= start_at_ps) begin
                  if (!generator.randomize()) begin status = TB_RANDOM_FAILED; break; end
                  target.drive(generator.value);
                end
              end
            end
            begin #(stop_at_ps - $time); end
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
