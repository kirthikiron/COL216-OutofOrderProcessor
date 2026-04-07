#pragma once
#include <iostream>
#include <vector>
#include <deque>
#include <climits>
#include <cstdint>
#include <string>
#include "Basics.h"

// LSQ entry (doubles as the RS for the load-store unit)
struct LSQEntry {
    bool valid = false;
    int  rob_tag = -1;
    OpCode op = OpCode::LW;
    int  pc = -1;
    int  issue_cycle = 0;

    // address base operand
    bool vj_ready = false;
    int  vj = 0;    // base register value
    int  qj = -1;   // producing ROB tag (-1 = ready)

    // store data operand
    bool vk_ready = false;
    int  vk = 0;    // store data value
    int  qk = -1;   // producing ROB tag (-1 = ready)

    int  imm = 0;   // offset
    int  dest = -1; // destination register (LW)
};

// Store buffer entry: a store that has executed but not yet committed
struct StoreBufferEntry {
    bool valid = false;
    int  rob_tag = -1;
    int  addr = -1;
    int  val  = 0;
    int  issue_cycle = 0; // for ordering checks
};

// In-flight execution tracker
struct LSQInFlight {
    int  rob_tag = -1;
    int  result  = 0;
    bool is_store = false;
    int  store_addr = 0;
    int  store_val  = 0;
    bool exception  = false;
    int  cycles_remaining = 0;
};

class LoadStoreQueue {
public:
    int latency;
    std::vector<LSQEntry> rs;       // reservation station / queue

    // Store buffer: stores that have executed (result computed) but not committed
    std::vector<StoreBufferEntry> store_buffer;

    bool has_result  = false;
    bool has_exception = false;
    int  result_tag  = -1;
    int  result_val  = 0;
    bool result_is_store = false;
    int  result_store_addr = 0;
    int  result_store_val  = 0;

    int store_data = 0; // legacy

    LoadStoreQueue() : latency(4) { store_buffer.resize(64); }
    LoadStoreQueue(int lat, int rs_size) : latency(lat) {
        rs.resize(rs_size);
        store_buffer.resize(rs_size * 2);
    }

    bool isFull() const {
        for (auto& e : rs) if (!e.valid) return false;
        return true;
    }

    int allocate(const LSQEntry& e) {
        for (int i = 0; i < (int)rs.size(); i++) {
            if (!rs[i].valid) {
                rs[i] = e;
                rs[i].valid = true;
                return i;
            }
        }
        return -1;
    }

    void capture(int tag, int val) {
        for (auto& entry : rs) {
            if (!entry.valid) continue;
            if (!entry.vj_ready && entry.qj == tag) {
                entry.vj = val; entry.vj_ready = true; entry.qj = -1;
            }
            if (!entry.vk_ready && entry.qk == tag) {
                entry.vk = val; entry.vk_ready = true; entry.qk = -1;
            }
        }
    }

    // Find the oldest valid entry (smallest issue_cycle)
    int findOldest() const {
        int best = -1;
        int best_cycle = INT32_MAX;
        for (int i = 0; i < (int)rs.size(); i++) {
            if (!rs[i].valid) continue;
            if (rs[i].issue_cycle < best_cycle) {
                best_cycle = rs[i].issue_cycle;
                best = i;
            }
        }
        return best;
    }

    // Pipeline tracking
    std::deque<LSQInFlight> pipeline;

    void executeCycle(std::vector<int>& Memory, int current_cycle) {
        has_result  = false;
        has_exception = false;
        result_tag  = -1;
        result_val  = 0;
        result_is_store = false;

        // Advance pipeline
        for (auto& inf : pipeline) inf.cycles_remaining--;

        // Check front of pipeline for completion
        if (!pipeline.empty() && pipeline.front().cycles_remaining <= 0) {
            auto& done = pipeline.front();
            has_result       = true;
            result_tag       = done.rob_tag;
            has_exception    = done.exception;
            result_is_store  = done.is_store;
            result_store_addr = done.store_addr;
            result_store_val  = done.store_val;
            result_val       = done.result;
            pipeline.pop_front();
        }

        // Try to issue the oldest entry
        int idx = findOldest();
        if (idx < 0) return;

        LSQEntry& e = rs[idx];
        ////////////////////
        // Prevent same-cycle issue for newly dispatched LSQ entries.
        if (e.issue_cycle >= current_cycle) return;

        // Check operand readiness
        if (!e.vj_ready) return;
        if (e.op == OpCode::SW && !e.vk_ready) return;

        int addr = e.vj + e.imm;
        bool exc = false;

        ////////////////////
        // Preserve store->load ordering for same-address hazards.
        if (e.op == OpCode::LW) {
            for (auto& pinf : pipeline) {
                if (pinf.is_store && pinf.store_addr == addr) return;
            }
        }

        LSQInFlight inf;
        inf.rob_tag = e.rob_tag;
        inf.cycles_remaining = latency > 0 ? latency - 1 : 0;

        if (e.op == OpCode::LW) {
            if (addr < 0 || addr >= (int)Memory.size()) {
                exc = true;
                inf.result = 0;
            } else {
                ////////////////////
                // Forward latest executed-but-uncommitted store value for same address.
                int forward_val = 0;
                int forward_cycle = INT32_MIN;
                bool have_forward = false;
                for (auto& sb : store_buffer) {
                    if (!sb.valid) continue;
                    if (sb.addr == addr && sb.issue_cycle > forward_cycle) {
                        forward_val = sb.val;
                        forward_cycle = sb.issue_cycle;
                        have_forward = true;
                    }
                }
                inf.result = have_forward ? forward_val : Memory[addr];
            }
            inf.is_store = false;
        } else { // SW
            if (addr < 0 || addr >= (int)Memory.size()) {
                exc = true;
            }
            inf.is_store   = true;
            inf.store_addr = addr;
            inf.store_val  = e.vk;
            inf.result     = 0;
            // Add to store buffer immediately when store executes
            // (will be cleaned up at commit)
            if (!exc) {
                for (auto& sb : store_buffer) {
                    if (!sb.valid) {
                        sb.valid      = true;
                        sb.rob_tag    = e.rob_tag;
                        sb.addr       = addr;
                        sb.val        = e.vk;
                        sb.issue_cycle = e.issue_cycle;
                        break;
                    }
                }
            }
        }
        inf.exception = exc;
        pipeline.push_back(inf);
        e.valid = false; // free RS slot
    }

    // Called at commit for a store: remove from store buffer
    void commitStore(int rob_tag) {
        for (auto& sb : store_buffer) {
            if (sb.valid && sb.rob_tag == rob_tag) {
                sb.valid = false;
                return;
            }
        }
    }

    // Flush everything
    void flush() {
        for (auto& e : rs) e.valid = false;
        for (auto& sb : store_buffer) sb.valid = false;
        pipeline.clear();
        has_result = false;
    }
};
