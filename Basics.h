#pragma once
#include <string>

enum class OpCode { ADD, SUB, ADDI, MUL, DIV, REM, LW, SW, BEQ, BNE, BLT, BLE, J, SLT, SLTI, AND, OR, XOR, ANDI, ORI, XORI };
enum class UnitType { ADDER, MULTIPLIER, DIVIDER, LOADSTORE, BRANCH, LOGIC };

struct Instruction {
    OpCode op;
    int dest;   // destination register (-1 if none)
    int src1;   // source register 1 (-1 if none)
    int src2;   // source register 2 (-1 if none)
    int imm;    // immediate value
    int pc;     // program counter of this instruction
};

struct ProcessorConfig {
    int num_regs = 32;
    int rob_size = 64;
    int mem_size = 1024;

    int logic_lat = 1;
    int add_lat = 2;
    int mul_lat = 4;
    int div_lat = 5;
    int mem_lat = 4;

    int logic_rs_size = 4;
    int adder_rs_size = 4;
    int mult_rs_size = 2;
    int div_rs_size = 2;
    int br_rs_size = 2;
    int lsq_rs_size = 32;
};

// ROB Entry
struct ROBEntry {
    bool valid = false;        // entry is in use
    bool ready = false;        // result is computed
    bool has_exception = false;// instruction raised exception
    int  dest = -1;            // architectural destination register (-1 = none / store / branch)
    int  value = 0;            // computed result
    int  pc = -1;              // PC of instruction
    OpCode op = OpCode::ADD;   // opcode (needed for commit logic)

    // branch-specific
    bool is_branch = false;
    int  branch_imm = 0;       // immediate offset for branch target
    bool predicted_taken = false;
    int  predicted_target = -1;
    bool actual_taken = false;
    int  actual_target = -1;

    // store-specific
    bool is_store = false;
    int  store_addr = 0;
    int  store_val = 0;
};

// Reservation Station Entry
struct RSEntry {
    bool valid = false;     // slot occupied

    int  rob_tag = -1;      // ROB tag assigned to this instruction

    // operand 1
    bool vj_ready = false;
    int  vj = 0;            // value of operand 1
    int  qj = -1;           // ROB tag producing operand 1 (-1 = ready)

    // operand 2
    bool vk_ready = false;
    int  vk = 0;            // value of operand 2
    int  qk = -1;           // ROB tag producing operand 2 (-1 = ready)

    int  imm = 0;           // immediate
    int  dest = -1;         // destination architectural register
    OpCode op = OpCode::ADD;
    int  pc = -1;           // PC for branch / issue-order tracking

    int  issue_cycle = 0;   // cycle when issued (for age-based selection)
};
