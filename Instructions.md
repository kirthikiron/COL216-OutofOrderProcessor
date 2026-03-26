# Out-of-Order Processor — Implementation Guide

This document explains how the simulator is implemented across all source files. It is meant to be read alongside the code.

---

## File Overview


| File                | Role                                                                           |
| ------------------- | ------------------------------------------------------------------------------ |
| `Basics.h`          | Shared data structures:`Instruction`, `ROBEntry`, `RSEntry`, `ProcessorConfig` |
| `BranchPredictor.h` | Per-instruction 2-bit saturating counter predictor                             |
| `ExecutionUnit.h`   | Pipelined ALU / branch execution unit with its own reservation station         |
| `LoadStoreQueue.h`  | In-order load/store unit with a store buffer for forwarding                    |
| `Processor.h`       | Top-level processor: fetch, decode, execute, commit, program loading           |
| `main.cpp`          | Entry point (provided, unchanged)                                              |

---

## Basics.h — Shared Data Structures

### `Instruction`

The internal representation of a decoded instruction. Fields:

- `op` — opcode enum (ADD, SUB, LW, BEQ, etc.)
- `dest`, `src1`, `src2` — architectural register indices (-1 if unused)
- `imm` — immediate value or pre-computed branch offset (label resolved to PC-relative integer at load time)
- `pc` — the program counter value of this instruction

### `ROBEntry`

One slot in the circular Reorder Buffer. The important fields are:

- `valid` / `ready` — whether the slot is in use and whether the result has been computed
- `has_exception` — set by the execution unit if the instruction faulted
- `dest` — architectural register to write at commit (-1 for stores/branches)
- `value` — computed result, written to ARF at commit
- `is_branch`, `branch_imm`, `predicted_taken`, `predicted_target`, `actual_taken`, `actual_target` — everything the commit stage needs to check branch correctness and update the predictor
- `is_store`, `store_addr`, `store_val` — for stores, the address and data are stored here so the commit stage can write memory at the right moment

### `RSEntry`

One slot in any execution unit's reservation station. Follows the standard Tomasulo naming:

- `vj` / `vk` — operand values once ready
- `qj` / `qk` — ROB tags of the instructions still producing the operands (-1 means the value is already in `vj`/`vk`)
- `vj_ready` / `vk_ready` — true when the operand value is available
- `rob_tag` — the ROB slot allocated for this instruction (used when broadcasting the result)
- `issue_cycle` — the clock cycle when this entry was placed in the RS, used to pick the oldest-ready entry for execution

### `ProcessorConfig`

A plain struct of integers controlling all hardware sizes and latencies. Passed to `Processor` at construction. Default values match the assignment spec.

---

## BranchPredictor.h

Implements a **per-instruction 2-bit saturating counter** keyed by instruction PC, stored in an `unordered_map<int, int>`.

### States

```
0 — Strongly Taken     (predict taken)
1 — Weakly Taken       (predict taken)
2 — Weakly Not-Taken   (predict not taken)
3 — Strongly Not-Taken (predict not taken)
```

New PCs start at state 0 (strongly taken).

### `predict(pc, imm, op)`

Returns the predicted next PC. For unconditional jumps (`J`) it always returns `pc + imm`. For conditional branches it checks the counter state: if 0 or 1, predicts taken and returns `pc + imm`; otherwise predicts not-taken and returns `pc + 1`.

### `update(pc, actual_target, taken, was_correct)`

Called during the **commit stage** of a branch. Increments `total_branches` and conditionally `correct_predictions`, then advances the counter state according to the spec transitions:

- Taken: 1→0, 2→1, 3→2 (state 0 stays 0)
- Not-taken: 0→1, 1→2, 2→3 (state 3 stays 3)

---

## ExecutionUnit.h

### Structure

Each `ExecutionUnit` owns:

- A `std::vector<RSEntry> rs` — the reservation station for this unit
- A `std::deque<InFlightEntry> pipeline` — the in-flight pipeline stages
- Result fields (`has_result`, `result_tag`, `result_val`, `has_exception`) that are set for exactly one cycle when an instruction completes

### `executeCycle(current_cycle)`

Called once per clock cycle. It does three things in order:

1. **Advance the pipeline** — decrements `cycles_remaining` on every in-flight entry.
2. **Check for completion** — if the front entry's `cycles_remaining` reaches 0, it is popped and its result fields are set. Only one result is produced per cycle (the front of the deque).
3. **Issue a new instruction** — calls `findOldestReady()` to find the RS entry with the smallest `issue_cycle` where both operands are ready. That entry is removed from the RS immediately and pushed onto the back of the pipeline with `cycles_remaining = latency`. This means a new instruction starts every cycle as long as something is ready, giving true pipeline overlap.

### `compute(entry, exc)`

A static helper that evaluates the arithmetic or logical result from the RS entry's operand values and opcode. For operations that can overflow (ADD, SUB, ADDI, MUL, DIV, REM) it promotes to `long long` and sets `exc = true` if the result falls outside the 32-bit signed range. Division and remainder by zero also set `exc`. Logic operations (AND, OR, XOR, etc.) never raise exceptions. Branch instructions return 1 (taken) or 0 (not-taken) as their result — the actual target is computed later in `broadcastOnCDB`.

### `capture(tag, val)`

Called during CDB broadcast. Scans the RS and fills in `vj` or `vk` for any entry whose `qj` or `qk` matches the given tag, then marks the operand ready.

### Unit Indices

The `units` vector in `Processor` is indexed as: 0 = Adder, 1 = Multiplier, 2 = Divider, 3 = Branch, 4 = Logic.

---

## LoadStoreQueue.h

The LSQ is different from the other execution units in one critical way: **it only issues instructions in program order** (oldest first, no out-of-order issue). This is because memory addresses are not renamed like registers, so a later load to the same address as an earlier store must always see the store's value.

### Store Buffer

When a store finishes execution it is placed into a `store_buffer` (a fixed-size array of `StoreBufferEntry`). Each entry holds `rob_tag`, `addr`, `val`, and `issue_cycle`. The store buffer entry is only cleared when the store's ROB entry commits (via `commitStore(rob_tag)`), at which point the value is also written to `Memory[]`. This ensures `Memory[]` always reflects only committed state.

### Store-to-Load Forwarding

When a load is about to execute, before reading `Memory[addr]` it scans the store buffer for any entry with a matching address. If found, it uses that value instead of the stale memory contents. This is how a load that comes after a store in program order sees the correct value even though the store hasn't committed yet. The forwarding logic picks the most recently issued matching store (highest `issue_cycle`) in case multiple stores hit the same address.

### Load Stalling Behind Stores

A load will not issue if there is any store currently in-flight in the LSQ pipeline (i.e., a store that has issued but whose result hasn't been recorded in the store buffer yet). It also will not issue if there is any older store still sitting in the RS waiting for its operands. This prevents a load from overtaking a store to the same address whose address isn't even known yet.

### `executeCycle(Memory)`

Works similarly to `ExecutionUnit::executeCycle`: advances the pipeline, checks for a completed entry at the front, then tries to issue the oldest RS entry (subject to the stall rules above).

---

## Processor.h

This is the main class. It ties all components together and implements the four pipeline stages.

### Key Members

- `pc` — the speculative program counter, updated by fetch using branch predictions
- `ARF` — the architectural register file, only written at commit
- `Memory` — the memory array, only written at commit (for stores)
- `RAT` — Register Alias Table: `RAT[r]` holds the ROB tag of the most recent instruction writing register `r`, or -1 if the register is up-to-date in the ARF
- `ROB` — circular reorder buffer, indexed by `rob_head` (oldest, next to commit) and `rob_tail` (next free slot)
- `units[5]` — the five execution units (Adder, Multiplier, Divider, Branch, Logic)
- `lsq` — pointer to the LoadStoreQueue
- `bp` — the BranchPredictor
- `fd_reg` / `di_reg` — pipeline registers between Fetch→Decode and Decode→Issue

### Program Loading (`loadProgram`)

The loader does two passes over the assembly file:

**Pass 1** reads line by line, stripping comments (everything after `#`) and whitespace. It handles three kinds of lines:

- Lines starting with `.` are memory declarations (`.A: 1 2 3`). The values are written sequentially into `Memory[]` starting at `mem_offset`, and the label name is mapped to its base address in `mem_labels`.
- Lines where a word ending in `:` appears before any space are instruction labels. The label is mapped to the current instruction count in `labels`. If there is remaining text on the same line it is treated as an instruction.
- Everything else is an instruction line, recorded with its index.

**Pass 2** calls `parseInstruction` for each recorded line, populating `inst_memory`.

### `parseInstruction`

Tokenises the line by whitespace, strips trailing commas, and dispatches on the opcode string. Register operands are parsed from strings like `x5`. Immediates are resolved by `resolveImm`: if the token looks like a number it is parsed directly; if it matches an instruction label the offset is computed as `label_pc - current_pc` (PC-relative); if it matches a memory label the absolute memory index is used. Memory operands like `A(x1)` are split at `(` and `)` to extract the label/offset and base register separately.

### The Four Pipeline Stages

#### `stageFetch`

Fetches the instruction at `pc` into `fd_reg`. Stalls if `di_reg` already holds an instruction waiting to issue. Advances `pc` using the branch predictor: unconditional jumps go directly to `pc + imm`; conditional branches use `bp.predict`; all other instructions use `pc + 1`. Sets `fetch_done` when `pc` goes past the end of `inst_memory`.

#### `stageDecode`

Moves the instruction from `fd_reg` into `di_reg`, then tries to issue it to the appropriate unit.

**Stall conditions**: if the ROB is full, or the target unit's RS is full, `di_reg.stall` is set and the stage returns without issuing. The instruction stays in `di_reg` until resources are free. Fetch is also stalled while `di_reg` holds an instruction.

**Unconditional jump (`J`)**: handled entirely in decode. A ROB entry is allocated and immediately marked ready with `actual_taken = true` and `actual_target = pc + imm`. No RS entry is created.

**All other instructions**: a ROB entry is allocated. Operands are resolved via `resolveOperand`: if `RAT[reg] == -1` the value comes straight from `ARF`; if the RAT points to a ready ROB entry the value is taken from there; otherwise the ROB tag is recorded in `qj`/`qk` and the operand is marked not-ready. The RS entry is filled and inserted into the appropriate unit's RS (or the LSQ). `RAT[dest]` is updated to the new ROB tag.

#### `stageExecuteAndBroadcast`

Calls `executeCycle` on every execution unit and on the LSQ, then calls `broadcastOnCDB`.

`broadcastOnCDB` collects all results produced this cycle (at most one per unit). For each result it updates the corresponding ROB entry (`ready = true`, `value`, `has_exception`). For branch results, it also sets `actual_taken` and `actual_target` using the taken/not-taken value (1 or 0) and the `branch_imm` stored in the ROB entry. For store results it copies `store_addr` and `store_val` into the ROB. It then calls `capture` on every RS and the LSQ so waiting operands can be forwarded.

#### `stageCommit`

Inspects the ROB head. If it is not valid or not ready, the stage does nothing. Otherwise:

- **Exception**: sets `Processor::exception = true`, sets `pc` to the faulting instruction's PC, flushes the pipeline, and sets `halted = true`.
- **Branch**: checks whether `predicted_taken == actual_taken` and the predicted target matches the actual target. Calls `bp.update` regardless. On a misprediction, sets `pc` to the correct next instruction, retires the branch's ROB entry, then flushes everything else.
- **Store**: writes `Memory[store_addr] = store_val` and calls `lsq->commitStore` to remove the entry from the store buffer.
- **Register write**: writes `ARF[dest] = value`. Clears `RAT[dest]` back to -1 only if it still points to this ROB entry (a later instruction for the same register may have since updated the RAT).
- `x0` is always forced to 0 after any write.

The ROB head pointer is advanced and `rob_count` decremented.

### `flush`

Clears `fd_reg`, `di_reg`, the entire ROB, the RAT, all RS entries and pipelines inside every execution unit, and the entire LSQ (RS, pipeline, and store buffer). Used after branch mispredictions and exceptions.

### `step`

Runs one clock cycle. Stages are called in reverse order (Commit → Execute → Decode → Fetch) so that a commit in the same cycle does not accidentally allow the freed ROB slot to be re-used by decode in the same cycle. Returns `false` when `fetch_done` is true and the ROB, pipeline registers, and all unit pipelines are empty.

---

## Instruction Encoding Conventions


| Instruction        | dest | src1 | src2 | imm                  |
| ------------------ | ---- | ---- | ---- | -------------------- |
| `add x1, x2, x3`   | x1   | x2   | x3   | —                   |
| `addi x1, x2, 5`   | x1   | x2   | —   | 5                    |
| `lw x1, A(x2)`     | x1   | x2   | —   | base address of A    |
| `sw x1, A(x2)`     | —   | x2   | x1   | base address of A    |
| `beq x1, x2, loop` | —   | x1   | x2   | loop_pc − branch_pc |
| `j loop`           | —   | —   | —   | loop_pc − j_pc      |

For `sw`, `src2` holds the data register and `src1` holds the base address register. The `imm` for branches and jumps is a PC-relative offset pre-computed at load time so the execute stage never needs to look up labels.

---

## Exception Handling (Precise)

All exceptions are detected at the end of the last execution cycle of the faulting instruction. The execution unit sets `has_exception = true` when broadcasting on the CDB, which gets stored in the ROB entry. The exception does **not** affect the processor's `exception` bit at that point. It only propagates when that ROB entry reaches the head of the ROB and commits. At that moment:

1. `Processor::exception` is set.
2. `pc` is set to the PC of the faulting instruction.
3. The pipeline is fully flushed.
4. `halted` is set so `step()` returns `false` immediately on the next call.

This guarantees that all instructions before the faulting one have already committed (their state is in `ARF`/`Memory`), and no instructions after it have any effect.

Exceptions are raised by: integer overflow (ADD, SUB, ADDI, MUL, DIV, REM), division or remainder by zero (DIV, REM), and out-of-bounds memory access (LW, SW).

---

## Branch Misprediction Recovery

When a conditional branch commits and is found to have been mispredicted:

1. The correct next PC is computed: `branch_pc + branch_imm` if taken, `branch_pc + 1` if not taken.
2. `bp.update` is called to update the predictor state.
3. The branch's own ROB entry is retired (head advanced, count decremented).
4. `flush()` is called, which wipes all speculative state: pipeline registers, the rest of the ROB, the RAT, all RS entries, and the LSQ.
5. On the next cycle, fetch restarts from the correct PC.

The branch predictor is updated at commit for every branch, whether or not a misprediction occurred.

---

## Memory Model

`Memory` is a zero-initialised `std::vector<int>` of size `mem_size`. It behaves like a flat integer array indexed from 0. Memory labels declared with `.LABEL: v1 v2 ...` fill the array sequentially across all declarations (`.A: 1 2 3` followed by `.B: 4 5` gives `Memory = [1,2,3,4,5,...]`). References to a label in `lw`/`sw` instructions are resolved to the base index of that label at load time. Accessing an address outside `[0, mem_size)` raises an exception.

The `Memory` array is only ever written during the **commit stage** of a store instruction, preserving the precise-exception guarantee: if an exception occurs before a store commits, memory is untouched.
