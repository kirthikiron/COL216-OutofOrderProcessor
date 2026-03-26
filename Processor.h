#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <climits>
#include "Basics.h"
#include "BranchPredictor.h"
#include "ExecutionUnit.h"
#include "LoadStoreQueue.h"

class Processor {
public:
    int pc;
    int clock_cycle;

    // Pipeline registers
    struct FetchDecodeReg {
        bool valid = false;
        Instruction inst;
    } fd_reg;

    struct DecodeIssueReg {
        bool valid = false;
        Instruction inst;
        bool stall = false;
    } di_reg;

    std::vector<Instruction> inst_memory;

    // Architectural state (do not change)
    std::vector<int> ARF;
    std::vector<int> Memory;
    bool exception = false;

    // Register Alias Table: maps architectural register -> ROB tag (-1 = use ARF)
    std::vector<int> RAT;

    // Reorder Buffer (circular)
    std::vector<ROBEntry> ROB;
    int rob_head = 0; // oldest entry (next to commit)
    int rob_tail = 0; // next free slot
    int rob_count = 0;

    // Execution units
    std::vector<ExecutionUnit> units; // index: ADDER=0, MUL=1, DIV=2, BRANCH=3, LOGIC=4
    LoadStoreQueue* lsq;
    BranchPredictor bp;

    // Config
    ProcessorConfig cfg;

    // Halt flag
    bool halted = false;
    bool fetch_done = false; // no more instructions to fetch

    Processor(ProcessorConfig& config) : cfg(config) {
        pc = 0;
        clock_cycle = 0;
        ARF.resize(config.num_regs, 0);
        Memory.resize(config.mem_size, 0);
        RAT.resize(config.num_regs, -1);
        ROB.resize(config.rob_size);

        // Instantiate Hardware Units (index corresponds to UnitType)
        // 0: ADDER
        units.emplace_back(UnitType::ADDER,      config.add_lat,   config.adder_rs_size);
        // 1: MULTIPLIER
        units.emplace_back(UnitType::MULTIPLIER, config.mul_lat,   config.mult_rs_size);
        // 2: DIVIDER
        units.emplace_back(UnitType::DIVIDER,    config.div_lat,   config.div_rs_size);
        // 3: BRANCH
        units.emplace_back(UnitType::BRANCH,     config.add_lat,   config.br_rs_size);
        // 4: LOGIC
        units.emplace_back(UnitType::LOGIC,      config.logic_lat, config.logic_rs_size);

        lsq = new LoadStoreQueue(config.mem_lat, config.lsq_rs_size);
    }

    ~Processor() { delete lsq; }

    // -------------------------------------------------------------------------
    // Program loading / preprocessing
    // -------------------------------------------------------------------------
    void loadProgram(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) throw std::runtime_error("Cannot open file");

        std::unordered_map<std::string, int> labels;    // label -> PC index
        std::unordered_map<std::string, int> mem_labels;// memory label -> base address
        std::vector<std::string> raw_lines;

        int mem_offset = 0;
        // First pass: collect memory declarations and instruction labels
        std::vector<std::pair<int,std::string>> inst_lines; // (line_index, line)

        int inst_count = 0;
        std::string line;
        while (std::getline(file, line)) {
            // strip comments
            auto cpos = line.find('#');
            if (cpos != std::string::npos) line = line.substr(0, cpos);
            // trim
            while (!line.empty() && (line.front()==' '||line.front()=='\t')) line.erase(line.begin());
            while (!line.empty() && (line.back()==' '||line.back()=='\t'||line.back()=='\r')) line.pop_back();
            if (line.empty()) continue;

            // Memory allocation: .LABEL: v1 v2 ...
            if (line[0] == '.') {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string lname = line.substr(1, colon-1);
                    mem_labels[lname] = mem_offset;
                    std::istringstream ss(line.substr(colon+1));
                    int v;
                    while (ss >> v) {
                        if (mem_offset < (int)Memory.size())
                            Memory[mem_offset++] = v;
                    }
                }
                continue;
            }

            // Instruction label: "labelname:"
            auto colon = line.find(':');
            if (colon != std::string::npos && line.find(' ') > colon) {
                // It's a label (no space before colon)
                std::string lname = line.substr(0, colon);
                labels[lname] = inst_count;
                // Rest of line after colon might be an instruction
                std::string rest = line.substr(colon+1);
                while (!rest.empty() && (rest.front()==' '||rest.front()=='\t')) rest.erase(rest.begin());
                if (!rest.empty()) {
                    inst_lines.push_back({inst_count, rest});
                    inst_count++;
                }
                continue;
            }

            inst_lines.push_back({inst_count, line});
            inst_count++;
        }

        // Second pass: parse instructions
        inst_memory.resize(inst_count);
        for (auto& p : inst_lines) {
            int idx = p.first;
            std::string iline = p.second;
            inst_memory[idx] = parseInstruction(iline, idx, labels, mem_labels);
}
    }

    // -------------------------------------------------------------------------
    // Instruction parser
    // -------------------------------------------------------------------------
    Instruction parseInstruction(const std::string& line, int pc_val,
                                  std::unordered_map<std::string,int>& labels,
                                  std::unordered_map<std::string,int>& mem_labels)
    {
        Instruction inst;
        inst.pc   = pc_val;
        inst.dest = -1; inst.src1 = -1; inst.src2 = -1; inst.imm = 0;

        std::istringstream ss(line);
        std::string opstr;
        ss >> opstr;

        // Remove trailing comma helper
        auto stripComma = [](std::string& s) {
            if (!s.empty() && s.back() == ',') s.pop_back();
        };

        auto parseReg = [](const std::string& s) -> int {
            // s = "x5" or "x5,"
            std::string t = s;
            if (!t.empty() && t.back()==',') t.pop_back();
            if (t[0]=='x') return std::stoi(t.substr(1));
            return -1;
        };

        // Resolve label or integer offset
        auto resolveImm = [&](const std::string& s, int cur_pc) -> int {
            // Could be a label (branch target) or integer
            if (s.empty()) return 0;
            // check if it starts with a digit or minus
            if (std::isdigit(s[0]) || s[0]=='-' || s[0]=='+') return std::stoi(s);
            // It's a label
            if (labels.count(s)) return labels[s] - cur_pc;
            if (mem_labels.count(s)) return mem_labels[s];
            return 0;
        };

        if (opstr == "add")  { inst.op = OpCode::ADD;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "sub")  { inst.op = OpCode::SUB;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "addi") { inst.op = OpCode::ADDI;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.imm=resolveImm(c,0); }
        else if (opstr == "slt")  { inst.op = OpCode::SLT;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "slti") { inst.op = OpCode::SLTI;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.imm=resolveImm(c,0); }
        else if (opstr == "mul")  { inst.op = OpCode::MUL;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "div")  { inst.op = OpCode::DIV;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "rem")  { inst.op = OpCode::REM;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "and")  { inst.op = OpCode::AND;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "or")   { inst.op = OpCode::OR;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "xor")  { inst.op = OpCode::XOR;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.src2=parseReg(c); }
        else if (opstr == "andi") { inst.op = OpCode::ANDI;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.imm=resolveImm(c,0); }
        else if (opstr == "ori")  { inst.op = OpCode::ORI;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.imm=resolveImm(c,0); }
        else if (opstr == "xori") { inst.op = OpCode::XORI;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.dest=parseReg(a); inst.src1=parseReg(b); inst.imm=resolveImm(c,0); }
        else if (opstr == "lw") {
            inst.op = OpCode::LW;
            std::string a, memop; ss>>a>>memop; stripComma(a);
            inst.dest = parseReg(a);
            // memop = "LABEL(xN)" or "offset(xN)"
            parseMem(memop, inst.src1, inst.imm, labels, mem_labels);
        }
        else if (opstr == "sw") {
            inst.op = OpCode::SW;
            std::string a, memop; ss>>a>>memop; stripComma(a);
            inst.src2 = parseReg(a); // data register
            // memop = "LABEL(xN)" or "offset(xN)"
            parseMem(memop, inst.src1, inst.imm, labels, mem_labels);
        }
        else if (opstr == "beq") { inst.op = OpCode::BEQ;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.src1=parseReg(a); inst.src2=parseReg(b); inst.imm=resolveImm(c,pc_val); }
        else if (opstr == "bne") { inst.op = OpCode::BNE;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.src1=parseReg(a); inst.src2=parseReg(b); inst.imm=resolveImm(c,pc_val); }
        else if (opstr == "blt") { inst.op = OpCode::BLT;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.src1=parseReg(a); inst.src2=parseReg(b); inst.imm=resolveImm(c,pc_val); }
        else if (opstr == "ble") { inst.op = OpCode::BLE;
            std::string a,b,c; ss>>a>>b>>c; stripComma(a);stripComma(b);
            inst.src1=parseReg(a); inst.src2=parseReg(b); inst.imm=resolveImm(c,pc_val); }
        else if (opstr == "j") {
            inst.op = OpCode::J;
            std::string c; ss>>c;
            inst.imm = resolveImm(c, pc_val);
        }

        return inst;
    }

    void parseMem(const std::string& memop, int& base_reg, int& offset,
                  std::unordered_map<std::string,int>& labels,
                  std::unordered_map<std::string,int>& mem_labels)
    {
        // Format: LABEL(xN) or imm(xN)
        auto lp = memop.find('(');
        auto rp = memop.find(')');
        if (lp == std::string::npos) { base_reg = 0; offset = 0; return; }
        std::string off_str = memop.substr(0, lp);
        std::string reg_str = memop.substr(lp+1, rp-lp-1);
        // parse base register
        if (!reg_str.empty() && reg_str[0]=='x') base_reg = std::stoi(reg_str.substr(1));
        else base_reg = 0;
        // parse offset / label
        if (off_str.empty()) { offset = 0; return; }
        if (std::isdigit(off_str[0]) || off_str[0]=='-') {
            offset = std::stoi(off_str);
        } else if (mem_labels.count(off_str)) {
            offset = mem_labels[off_str];
        } else if (labels.count(off_str)) {
            offset = labels[off_str];
        } else {
            offset = 0;
        }
    }

    // -------------------------------------------------------------------------
    // ROB helpers
    // -------------------------------------------------------------------------
    bool robFull() const { return rob_count == (int)ROB.size(); }

    int robAllocate(const Instruction& inst, int predicted_pc) {
        int tag = rob_tail;
        ROBEntry& e = ROB[tag];
        e.valid        = true;
        e.ready        = false;
        e.has_exception= false;
        e.dest         = inst.dest;
        e.value        = 0;
        e.pc           = inst.pc;
        e.op           = inst.op;
        e.is_branch    = (inst.op==OpCode::BEQ||inst.op==OpCode::BNE||
                          inst.op==OpCode::BLT||inst.op==OpCode::BLE||inst.op==OpCode::J);
        e.branch_imm   = inst.imm;
        e.is_store     = (inst.op==OpCode::SW);
        e.predicted_target = predicted_pc;
        // predicted_taken: for J always true; for others from predictor
        if (inst.op == OpCode::J) {
            e.predicted_taken = true;
        } else {
            e.predicted_taken = bp.predictsTaken(inst.pc);
        }
        rob_tail = (rob_tail + 1) % (int)ROB.size();
        rob_count++;
        return tag;
    }

    // -------------------------------------------------------------------------
    // Operand resolution helpers (RAT lookup)
    // -------------------------------------------------------------------------
    void resolveOperand(int reg, bool& ready, int& val, int& tag) {
        if (reg < 0) { ready = true; val = 0; tag = -1; return; }
        int rat_tag = RAT[reg];
        if (rat_tag == -1) {
            ready = true;
            val   = ARF[reg];
            tag   = -1;
        } else {
            // Check if ROB entry is already ready
            if (ROB[rat_tag].ready) {
                ready = true;
                val   = ROB[rat_tag].value;
                tag   = -1;
            } else {
                ready = false;
                val   = 0;
                tag   = rat_tag;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Unit index helper
    // -------------------------------------------------------------------------
    int unitIndex(UnitType t) const {
        switch(t) {
            case UnitType::ADDER:      return 0;
            case UnitType::MULTIPLIER: return 1;
            case UnitType::DIVIDER:    return 2;
            case UnitType::BRANCH:     return 3;
            case UnitType::LOGIC:      return 4;
            default: return -1;
        }
    }

    UnitType getUnitType(OpCode op) {
        switch(op) {
            case OpCode::ADD: case OpCode::SUB: case OpCode::ADDI:
            case OpCode::SLT: case OpCode::SLTI:
                return UnitType::ADDER;
            case OpCode::MUL:
                return UnitType::MULTIPLIER;
            case OpCode::DIV: case OpCode::REM:
                return UnitType::DIVIDER;
            case OpCode::BEQ: case OpCode::BNE: case OpCode::BLT: case OpCode::BLE:
                return UnitType::BRANCH;
            case OpCode::AND: case OpCode::OR:  case OpCode::XOR:
            case OpCode::ANDI:case OpCode::ORI: case OpCode::XORI:
                return UnitType::LOGIC;
            case OpCode::LW: case OpCode::SW:
                return UnitType::LOADSTORE;
            default:
                return UnitType::ADDER;
        }
    }

    // -------------------------------------------------------------------------
    // flush: clear all pipeline state, keep ARF/Memory
    // -------------------------------------------------------------------------
    void flush() {
        fd_reg.valid = false;
        di_reg.valid = false;
        di_reg.stall = false;

        // Clear ROB
        for (auto& e : ROB) e.valid = false;
        rob_head = 0; rob_tail = 0; rob_count = 0;

        // Clear RAT
        for (auto& r : RAT) r = -1;

        // Clear execution units
        for (auto& u : units) {
            for (auto& e : u.rs) e.valid = false;
            u.pipeline.clear();
            u.has_result = false;
        }

        // Clear LSQ
        lsq->flush();

        fetch_done = false;
    }

    // -------------------------------------------------------------------------
    int findImmForROB(const ROBEntry& re) {
        return re.branch_imm;
    }

    // -------------------------------------------------------------------------
    // broadcastOnCDB: after execute, propagate results to all RS and ROB
    // -------------------------------------------------------------------------
    void broadcastOnCDB() {
        // Collect all results from units and LSQ
        struct CDBResult { int tag; int val; bool exc; bool is_store; int store_addr; int store_val; };
        std::vector<CDBResult> results;

        for (auto& u : units) {
            if (u.has_result) {
                results.push_back({u.result_tag, u.result_val, u.has_exception, false, 0, 0});
            }
        }
        if (lsq->has_result) {
            results.push_back({lsq->result_tag, lsq->result_val, lsq->has_exception,
                                lsq->result_is_store, lsq->result_store_addr, lsq->result_store_val});
        }

        for (auto& r : results) {
            // Update ROB
            if (r.tag >= 0 && r.tag < (int)ROB.size() && ROB[r.tag].valid) {
                ROBEntry& re = ROB[r.tag];
                re.ready         = true;
                re.has_exception = r.exc;
                if (r.is_store) {
                    re.store_addr = r.store_addr;
                    re.store_val  = r.store_val;
                } else if (re.is_branch) {
                    // r.val is 1=taken, 0=not-taken for conditional branches
                    bool taken = (r.val != 0);
                    re.actual_taken  = taken;
                    re.actual_target = taken ? (re.pc + findImmForROB(re)) : (re.pc + 1);
                    re.value = r.val;
                } else {
                    re.value = r.val;
                }
            }
            // Broadcast to all execution unit RS
            for (auto& u : units) u.capture(r.tag, r.val);
            // Broadcast to LSQ
            lsq->capture(r.tag, r.val);
        }
    }

    // -------------------------------------------------------------------------
    // stageFetch
    // -------------------------------------------------------------------------
    void stageFetch() {
        if (fetch_done) return;
        // Stall if decode is holding an instruction
        if (di_reg.valid && di_reg.stall) return;
        // Also stall if the decode stage still has an un-issued instruction
        if (di_reg.valid) return;

        if (pc >= (int)inst_memory.size()) {
            fetch_done = true;
            return;
        }

        Instruction inst = inst_memory[pc];

        // For J: no prediction needed, directly compute target
        int next_pc;
        if (inst.op == OpCode::J) {
            next_pc = pc + inst.imm;
        } else if (inst.op == OpCode::BEQ || inst.op == OpCode::BNE ||
                   inst.op == OpCode::BLT || inst.op == OpCode::BLE) {
            next_pc = bp.predict(pc, inst.imm, inst.op);
        } else {
            next_pc = pc + 1;
        }

        fd_reg.valid = true;
        fd_reg.inst  = inst;
        // Advance PC based on prediction
        pc = next_pc;
    }

    // -------------------------------------------------------------------------
    // stageDecode: allocate ROB + RS
    // -------------------------------------------------------------------------
    void stageDecode() {
        // Move fd -> di if di is free
        if (!di_reg.valid || di_reg.stall) {
            if (fd_reg.valid) {
                di_reg.valid = true;
                di_reg.inst  = fd_reg.inst;
                di_reg.stall = false;
                fd_reg.valid = false;
            }
        }

        if (!di_reg.valid) return;

        Instruction& inst = di_reg.inst;

        // J is handled here directly (no execution unit needed)
        if (inst.op == OpCode::J) {
            // Need ROB entry; J has no dest and always commits immediately (mark ready)
            if (robFull()) { di_reg.stall = true; return; }
            int tag = robAllocate(inst, pc); // pc was already updated in fetch
            // J is always taken — mark ROB ready with no value
            ROB[tag].ready = true;
            ROB[tag].actual_taken = true;
            ROB[tag].actual_target = inst.pc + inst.imm;
            di_reg.valid = false;
            di_reg.stall = false;
            return;
        }

        UnitType ut = getUnitType(inst.op);
        bool use_lsq = (ut == UnitType::LOADSTORE);

        // Check ROB full
        if (robFull()) { di_reg.stall = true; return; }
        // Check RS full
        if (!use_lsq && units[unitIndex(ut)].isFull()) { di_reg.stall = true; return; }
        if (use_lsq && lsq->isFull()) { di_reg.stall = true; return; }

        // Allocate ROB entry
        int predicted_next = pc; // pc is already the predicted next after fetch
        int rob_tag = robAllocate(inst, predicted_next);

        // Build RSEntry / LSQEntry
        bool vj_ready, vk_ready;
        int  vj, vk, qj, qk;

        // Operand 1 (src1)
        if (inst.src1 >= 0) {
            resolveOperand(inst.src1, vj_ready, vj, qj);
        } else {
            vj_ready = true; vj = 0; qj = -1;
        }

        // Operand 2 (src2 or imm-based)
        bool uses_src2 = (inst.src2 >= 0);
        if (uses_src2) {
            resolveOperand(inst.src2, vk_ready, vk, qk);
        } else {
            vk_ready = true; vk = 0; qk = -1;
        }

        // Update RAT for destination register
        if (inst.dest > 0) { // never map x0
            RAT[inst.dest] = rob_tag;
        }

        if (!use_lsq) {
            RSEntry e;
            e.valid    = true;
            e.rob_tag  = rob_tag;
            e.op       = inst.op;
            e.dest     = inst.dest;
            e.pc       = inst.pc;
            e.imm      = inst.imm;
            e.vj_ready = vj_ready; e.vj = vj; e.qj = qj;
            e.vk_ready = vk_ready; e.vk = vk; e.qk = qk;
            e.issue_cycle = clock_cycle;
            units[unitIndex(ut)].allocate(e);
        } else {
            LSQEntry e;
            e.valid    = true;
            e.rob_tag  = rob_tag;
            e.op       = inst.op;
            e.dest     = inst.dest;
            e.pc       = inst.pc;
            e.imm      = inst.imm;
            e.vj_ready = vj_ready; e.vj = vj; e.qj = qj;
            e.vk_ready = vk_ready; e.vk = vk; e.qk = qk;
            e.issue_cycle = clock_cycle;
            lsq->allocate(e);
        }

        di_reg.valid = false;
        di_reg.stall = false;
    }

    // -------------------------------------------------------------------------
    // stageExecuteAndBroadcast
    // -------------------------------------------------------------------------
    void stageExecuteAndBroadcast() {
        // Run all execution units
        for (auto& u : units) u.executeCycle(clock_cycle);
        lsq->executeCycle(Memory);

        // Broadcast results on CDB
        broadcastOnCDB();
    }

    // -------------------------------------------------------------------------
    // stageCommit
    // -------------------------------------------------------------------------
    void stageCommit() {
        if (rob_count == 0) return;
        ROBEntry& head = ROB[rob_head];
        if (!head.valid || !head.ready) return;

        // Exception handling
        if (head.has_exception) {
            exception = true;
            pc = head.pc;
            flush();
            halted = true;
            return;
        }

        // Branch commit
        if (head.is_branch) {
            bool taken   = head.actual_taken;
            int  target  = head.actual_target;
            bool correct = (head.predicted_taken == taken) &&
                           (head.predicted_target == target ||
                            // if not taken, target doesn't matter as long as pc+1 was predicted
                            (!taken && head.predicted_target == head.pc + 1));

            // Compute correct next PC
                int correct_next;
                if (taken) correct_next = target;
                else       correct_next = head.pc + 1;

                // Update branch predictor
                bp.update(head.pc, target, taken, correct);

                if (!correct) {
                    // Flush and redirect
                    pc = correct_next;
                    // Retire this ROB entry first
                    head.valid = false;
                    rob_head = (rob_head + 1) % (int)ROB.size();
                    rob_count--;
                    flush();
                    return;
                }
        }

        // Store: write to memory and remove from store buffer
        if (head.is_store) {
            Memory[head.store_addr] = head.store_val;
            lsq->commitStore(rob_head);
        }

        // Register write
        if (head.dest > 0) {
            ARF[head.dest] = head.value;
            // Clear RAT only if this ROB entry is still the latest producer
            if (RAT[head.dest] == rob_head) RAT[head.dest] = -1;
        }
        // x0 is always 0
        ARF[0] = 0;

        head.valid = false;
        rob_head = (rob_head + 1) % (int)ROB.size();
        rob_count--;
    }

    // -------------------------------------------------------------------------
    // step(): execute one cycle
    // -------------------------------------------------------------------------
    bool step() {
        if (halted) return false;
        clock_cycle++;

        // Pipeline stages executed in reverse order to avoid same-cycle forwarding issues
        stageCommit();
        if (halted) return false;

        stageExecuteAndBroadcast();
        stageDecode();
        stageFetch();

        // Check termination: ROB empty, no instructions in-flight, fetch done
        bool pipeline_empty = (rob_count == 0) &&
                              !fd_reg.valid &&
                              !di_reg.valid;
        bool units_idle = true;
        for (auto& u : units) if (!u.pipeline.empty()) { units_idle = false; break; }
        if (!lsq->pipeline.empty()) units_idle = false;

        if (fetch_done && pipeline_empty && units_idle) return false;
        return true;
    }

    void dumpArchitecturalState() {
        std::cout << "\n=== ARCHITECTURAL STATE (CYCLE " << clock_cycle << ") ===\n";
        for (int i = 0; i < (int)ARF.size(); i++) {
            std::cout << "x" << i << ": " << std::setw(4) << ARF[i] << " | ";
            if ((i+1) % 8 == 0) std::cout << std::endl;
        }
        if (exception) {
            std::cout << "EXCEPTION raised by instruction " << pc + 1 << std::endl;
        }
        std::cout << "Branch Predictor Stats: " << bp.correct_predictions << "/" << bp.total_branches << " correct.\n";
    }
};
