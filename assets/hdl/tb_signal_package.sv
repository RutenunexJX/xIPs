package tb_signal_package;
  timeunit 1ps;
  timeprecision 1ps;

  typedef enum { TB_OK, TB_CANCELLED, TB_INVALID_ARGUMENT, TB_RANDOM_FAILED, TB_EXPIRED } status_t;

  class cancellation;
    bit requested = 0;
    function void cancel(); requested = 1; endfunction
  endclass

  // Implement in the calling testbench using its actual signal declarations.
  // wait_edge must block until one real clock edge; drive must update immediately.
  virtual class signal_driver #(type T = logic);
    pure virtual task wait_edge(input bit rising);
    pure virtual function void drive(input T value);
  endclass
endpackage
