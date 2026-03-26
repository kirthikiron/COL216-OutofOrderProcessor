#pragma once
#include "Basics.h"
#include <iostream>
#include <unordered_map>

// 2-bit saturating counter branch predictor, per-instruction (keyed by PC)
// States:
//   0 - Strongly Taken   (predict taken)
//   1 - Weakly Taken     (predict taken)
//   2 - Weakly Not-Taken (predict not taken)
//   3 - Strongly Not-Taken (predict not taken)
//
// Starting state: 0 (predict taken)
//
// Transitions:
//   State 1 -taken->     State 0
//   State 2 -taken->     State 1
//   State 3 -taken->     State 2
//   State 0 -not taken-> State 1
//   State 1 -not taken-> State 2
//   State 2 -not taken-> State 3
//   (all other cases: state unchanged)

class BranchPredictor {
public:
    int total_branches = 0;
    int correct_predictions = 0;

    // Returns the predicted next PC.
    // predict_taken => return current_pc + imm
    // predict_not_taken => return current_pc + 1
    int predict(int current_pc, int imm, OpCode op) {
        if (op == OpCode::J) {
            // unconditional jump — always taken, handled at decode
            return current_pc + imm;
        }
        int state = getState(current_pc);
        bool taken = (state == 0 || state == 1);
        if (taken) {
            return current_pc + imm;
        } else {
            return current_pc + 1;
        }
    }

    // Returns whether the predictor says "taken" for this PC
    bool predictsTaken(int pc) {
        int state = getState(pc);
        return (state == 0 || state == 1);
    }

    // Called during commit. Updates state and stats.
    void update(int pc, int actual_target, bool taken, bool was_correct) {
        total_branches++;
        if (was_correct) correct_predictions++;
        int& state = counter[pc];
        if (taken) {
            // transitions on taken: 1->0, 2->1, 3->2, 0->0 (unchanged)
            if (state == 1) state = 0;
            else if (state == 2) state = 1;
            else if (state == 3) state = 2;
            // state 0 stays 0
        } else {
            // transitions on not-taken: 0->1, 1->2, 2->3, 3->3 (unchanged)
            if (state == 0) state = 1;
            else if (state == 1) state = 2;
            else if (state == 2) state = 3;
            // state 3 stays 3
        }
    }

private:
    std::unordered_map<int, int> counter; // PC -> 2-bit state (0..3)

    int getState(int pc) {
        auto it = counter.find(pc);
        if (it == counter.end()) {
            counter[pc] = 0; // default: strongly taken
            return 0;
        }
        return it->second;
    }
};
