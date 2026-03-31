#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <deque>
#include <climits>
#include <cstdint>
#include "Basics.h"

// Represents one stage of an in-flight instruction inside the pipeline
struct InFlightEntry {
    int  rob_tag   = -1;
    int  result    = 0;
    bool exception = false;
    int  cycles_remaining = 0; // how many more cycles until done
};

class ExecutionUnit {
public:
    UnitType name;
    int latency;

    // Reservation station for this unit
    std::vector<RSEntry> rs;

    // Pipelined stages: each entry is one instruction in-flight
    // We allow one new issue per cycle; multiple in-flight at once
    std::deque<InFlightEntry> pipeline;

    // Result ready this cycle (to be broadcast on CDB)
    bool has_result  = false;
    int  result_tag  = -1;
    int  result_val  = 0;
    bool has_exception = false;

    ExecutionUnit() : latency(1) {}
    ExecutionUnit(UnitType t, int lat, int rs_size)
        : name(t), latency(lat)
    {
        rs.resize(rs_size);
    }

    // Called by CDB broadcast: update operands in RS
    void capture(int tag, int val) {
        for (auto& entry : rs) {
            if (!entry.valid) continue;
            if (!entry.vj_ready && entry.qj == tag) {
                entry.vj = val;
                entry.vj_ready = true;
                entry.qj = -1;
            }
            if (!entry.vk_ready && entry.qk == tag) {
                entry.vk = val;
                entry.vk_ready = true;
                entry.qk = -1;
            }
        }
    }

    // Allocate a free RS slot; returns index or -1 if full
    int allocate(const RSEntry& e) {
        for (int i = 0; i < (int)rs.size(); i++) {
            if (!rs[i].valid) {
                rs[i] = e;
                rs[i].valid = true;
                return i;
            }
        }
        return -1;
    }

    bool isFull() const {
        for (auto& e : rs) if (!e.valid) return false;
        return true;
    }

    // Find the oldest ready entry (by issue_cycle)
    int findOldestReady() const {
        int best = -1;
        int best_cycle = INT32_MAX;
        for (int i = 0; i < (int)rs.size(); i++) {
            if (!rs[i].valid) continue;
            if (!rs[i].vj_ready || !rs[i].vk_ready) continue;
            if (rs[i].issue_cycle < best_cycle) {
                best_cycle = rs[i].issue_cycle;
                best = i;
            }
        }
        return best;
    }

    // Compute result for an RS entry
    static int compute(const RSEntry& e, bool& exc) {
        exc = false;
        int a = e.vj, b = e.vk;
        const long long INT32_MAX_VAL = 2147483647LL;
        const long long INT32_MIN_VAL = -2147483648LL;
        switch (e.op) {
            case OpCode::ADD:  { long long r = (long long)a + b;  if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::SUB:  { long long r = (long long)a - b;  if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::ADDI: { long long r = (long long)a + e.imm; if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::SLT:  return (a < b) ? 1 : 0;
            case OpCode::SLTI: return (a < e.imm) ? 1 : 0;
            case OpCode::MUL:  { long long r = (long long)a * b;  if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::DIV:
                if (b == 0) { exc = true; return 0; }
                { long long r = (long long)a / b; if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::REM:
                if (b == 0) { exc = true; return 0; }
                { long long r = (long long)a % b; if (r > INT32_MAX_VAL || r < INT32_MIN_VAL) exc = true; return (int)r; }
            case OpCode::AND:  return a & b;
            case OpCode::OR:   return a | b;
            case OpCode::XOR:  return a ^ b;
            case OpCode::ANDI: return a & e.imm;
            case OpCode::ORI:  return a | e.imm;
            case OpCode::XORI: return a ^ e.imm;
            // Branch: compute target and taken flag (packed as result; handled specially)
            case OpCode::BEQ:  return (a == b) ? 1 : 0;
            case OpCode::BNE:  return (a != b) ? 1 : 0;
            case OpCode::BLT:  return (a <  b) ? 1 : 0;
            case OpCode::BLE:  return (a <= b) ? 1 : 0;
            default: return 0;
        }
    }

    // Called once per cycle: advance pipeline, issue new instruction if available
    // Returns true if a result is ready (stored in result_tag/result_val/has_exception)
    void executeCycle(int current_cycle) {
        has_result  = false;
        has_exception = false;
        result_tag  = -1;
        result_val  = 0;

        // Advance all in-flight entries
        for (auto& inf : pipeline) {
            inf.cycles_remaining--;
        }

        // Check if the front of the pipeline is done
        if (!pipeline.empty() && pipeline.front().cycles_remaining <= 0) {
            auto& done = pipeline.front();
            has_result    = true;
            result_tag    = done.rob_tag;
            result_val    = done.result;
            has_exception = done.exception;
            pipeline.pop_front();
        }

        // Issue the oldest ready RS entry into the pipeline (one per cycle)
        int idx = findOldestReady();
        if (idx >= 0) {
            RSEntry& e = rs[idx];
            bool exc = false;
            int res = compute(e, exc);
            InFlightEntry inf;
            inf.rob_tag          = e.rob_tag;
            inf.result           = res;
            inf.exception        = exc;
            
            //   inf.cycles_remaining = latency; // will be decremented next cycle
            inf.cycles_remaining = latency > 0? latency - 1 : 0; // execute in same cycle if latency=0
            pipeline.push_back(inf);
            e.valid = false; // free RS slot immediately on issue
        }
    }
};
