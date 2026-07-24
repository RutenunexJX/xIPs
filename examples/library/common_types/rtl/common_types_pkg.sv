package common_types_pkg;
    typedef logic [31:0] word_t;

    typedef enum logic [1:0] {
        IDLE,
        ACTIVE,
        ERROR
    } state_t;
endpackage
