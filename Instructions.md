# Out-of-Order Processor — Implementation Guide

# Out-of-Order Processor — Detailed Function and Workflow Reference

---

## Table of Contents

1. [Project Structure](#1-project-structure)
2. [Basics.h — Shared Data Structures](#2-basicsh--shared-data-structures)
3. [BranchPredictor.h — All Functions](#3-branchpredictorh--all-functions)
4. [ExecutionUnit.h — All Functions](#4-executionunith--all-functions)
5. [LoadStoreQueue.h — All Functions](#5-loadstorequeueh--all-functions)
6. [Processor.h — All Functions](#6-processorh--all-functions)
7. [Full Workflow: What Happens Each Cycle](#7-full-workflow-what-happens-each-cycle)
8. [Full Workflow: What Happens at Program Start](#8-full-workflow-what-happens-at-program-start)
9. [Full Workflow: Branch Misprediction](#9-full-workflow-branch-misprediction)
10. [Full Workflow: Exception](#10-full-workflow-exception)
11. [Data Flow Diagram](#11-data-flow-diagram)

---

## 1. Project Structure

```
Basics.h          — All shared structs and enums used across every file
BranchPredictor.h — 2-bit saturating counter branch predictor
ExecutionUnit.h   — Pipelined arithmetic/logic/branch execution unit
LoadStoreQueue.h  — In-order load/store unit with store buffer
Processor.h       — Top-level processor, pipeline stages, program loader
main.cpp          — Entry point (provided, do not modify)
```

Every `.h` file is included into `main.cpp` through the chain:

```
main.cpp -> Processor.h -> ExecutionUnit.h -> Basics.h
                        -> LoadStoreQueue.h -> Basics.h
                        -> BranchPredictor.h -> Basics.h
```

---

## 2. Basics.h — Shared Data Structures

This file defines the data types shared across all other files. Nothing here is executable — it is purely definitions.

---

### `enum class OpCode`

Lists every instruction the processor supports as named constants:

```
ADD, SUB, ADDI, MUL, DIV, REM       — arithmetic
LW, SW                               — memory
BEQ, BNE, BLT, BLE, J               — branches / jumps
SLT, SLTI                            — set-less-than (on adder unit)
AND, OR, XOR, ANDI, ORI, XORI        — bitwise logic
```

Using an enum class means you write `OpCode::ADD` instead of just `ADD`, which prevents name conflicts with other parts of the code.

---

### `enum class UnitType`

Names the six types of execution units:

```
ADDER, MULTIPLIER, DIVIDER, LOADSTORE, BRANCH, LOGIC
```

Used to identify which unit an instruction belongs to and to index into the `units` vector in `Processor`.

---

### `struct Instruction`

The internal representation of one decoded instruction. Created during program loading and stored in `inst_memory`.


| Field  | Type   | Meaning                                       |
| ------ | ------ | --------------------------------------------- |
| `op`   | OpCode | Which operation to perform                    |
| `dest` | int    | Destination register index, -1 if none        |
| `src1` | int    | First source register index, -1 if none       |
| `src2` | int    | Second source register index, -1 if none      |
| `imm`  | int    | Immediate value or PC-relative branch offset  |
| `pc`   | int    | The program counter value of this instruction |

For `sw`, the data register is stored in `src2` and the base address register is in `src1`. For branches and jumps, `imm` is pre-computed as a PC-relative offset at load time so execute never needs to look up labels.

---

### `struct ProcessorConfig`

A plain configuration struct whose fields control all hardware sizes and latencies. All fields have defaults matching the assignment spec. Passed to the `Processor` constructor.


| Field           | Default | Meaning                           |
| --------------- | ------- | --------------------------------- |
| `num_regs`      | 32      | Number of architectural registers |
| `rob_size`      | 64      | Number of ROB slots               |
| `mem_size`      | 1024    | Size of Memory array              |
| `logic_lat`     | 1       | Cycles for logic operations       |
| `add_lat`       | 2       | Cycles for add/sub/addi/slt       |
| `mul_lat`       | 4       | Cycles for multiply               |
| `div_lat`       | 5       | Cycles for divide/remainder       |
| `mem_lat`       | 4       | Cycles for load/store             |
| `logic_rs_size` | 4       | RS slots for logic unit           |
| `adder_rs_size` | 4       | RS slots for adder unit           |
| `mult_rs_size`  | 2       | RS slots for multiplier unit      |
| `div_rs_size`   | 2       | RS slots for divider unit         |
| `br_rs_size`    | 2       | RS slots for branch unit          |
| `lsq_rs_size`   | 32      | RS slots for load/store queue     |

---

### `struct ROBEntry`

One slot in the circular Reorder Buffer. Every instruction that enters the pipeline gets exactly one ROB entry, which stays alive until that instruction commits.


| Field              | Type   | Meaning                                                                                         |
| ------------------ | ------ | ----------------------------------------------------------------------------------------------- |
| `valid`            | bool   | Whether this slot is currently occupied                                                         |
| `ready`            | bool   | Whether the instruction has finished executing and its result is available                      |
| `has_exception`    | bool   | Whether the instruction caused a fault during execution                                         |
| `dest`             | int    | Architectural register to write at commit (-1 for stores/branches with no register destination) |
| `value`            | int    | The computed result, written to ARF at commit                                                   |
| `pc`               | int    | The PC of this instruction, used for exception reporting and branch checking                    |
| `op`               | OpCode | The opcode, needed by the commit stage to decide what action to take                            |
| `is_branch`        | bool   | True for BEQ, BNE, BLT, BLE, J                                                                  |
| `branch_imm`       | int    | The PC-relative offset, stored so the commit stage can compute the actual target                |
| `predicted_taken`  | bool   | What the branch predictor said at fetch time                                                    |
| `predicted_target` | int    | The PC the predictor sent fetch to                                                              |
| `actual_taken`     | bool   | What the branch actually did, filled in by broadcastOnCDB                                       |
| `actual_target`    | int    | The correct next PC, filled in by broadcastOnCDB                                                |
| `is_store`         | bool   | True for SW                                                                                     |
| `store_addr`       | int    | Memory address computed during execute, filled in by broadcastOnCDB                             |
| `store_val`        | int    | Data to write, filled in by broadcastOnCDB                                                      |

---

### `struct RSEntry`

One slot in any execution unit's reservation station. Uses the standard Tomasulo naming convention.


| Field         | Type   | Meaning                                                                                                   |
| ------------- | ------ | --------------------------------------------------------------------------------------------------------- |
| `valid`       | bool   | Whether this slot is occupied                                                                             |
| `rob_tag`     | int    | The ROB slot index for this instruction, used when broadcasting the result                                |
| `vj_ready`    | bool   | Whether operand 1's value is available                                                                    |
| `vj`          | int    | Value of operand 1 once ready                                                                             |
| `qj`          | int    | ROB tag of the instruction still producing operand 1 (-1 means vj is ready)                               |
| `vk_ready`    | bool   | Whether operand 2's value is available                                                                    |
| `vk`          | int    | Value of operand 2 once ready                                                                             |
| `qk`          | int    | ROB tag of the instruction still producing operand 2 (-1 means vk is ready)                               |
| `imm`         | int    | Immediate value (used by ADDI, ANDI, etc.)                                                                |
| `dest`        | int    | Destination architectural register                                                                        |
| `op`          | OpCode | The operation to perform                                                                                  |
| `pc`          | int    | PC of this instruction                                                                                    |
| `issue_cycle` | int    | The clock cycle when this entry was placed in the RS, used to select the oldest-ready entry for execution |

---

## 3. BranchPredictor.h — All Functions

The predictor uses a `std::unordered_map<int, int> counter` which maps each instruction's PC to its 2-bit state (0, 1, 2, or 3). New PCs start at state 0 automatically.

---

### `getState(int pc)` — private

```cpp
int getState(int pc)
```

Looks up the 2-bit counter state for the given PC. Uses `find` to check if an entry exists. If not found, inserts 0 (strongly taken, the starting state) and returns 0. If found, returns the current state.

Could be simplified to `return counter[pc]` since the `[]` operator also inserts 0 for missing keys, but the explicit check makes the intent clearer.

---

### `predict(int current_pc, int imm, OpCode op)`

```cpp
int predict(int current_pc, int imm, OpCode op)
```

Returns the predicted next PC.

- If `op == J` (unconditional jump): always returns `current_pc + imm` since jumps are never not-taken.
- For conditional branches: reads the counter state with `getState`. States 0 and 1 predict taken → returns `current_pc + imm`. States 2 and 3 predict not-taken → returns `current_pc + 1`.

Called during the **Fetch stage** to advance the PC speculatively.

---

### `predictsTaken(int pc)`

```cpp
bool predictsTaken(int pc)
```

Returns `true` if the predictor would predict this branch as taken (state 0 or 1), `false` otherwise. Called during the **Decode stage** when filling in the `predicted_taken` field of the ROB entry.

---

### `update(int pc, int actual_target, bool taken, bool was_correct)`

```cpp
void update(int pc, int actual_target, bool taken, bool was_correct)
```

Called during the **Commit stage** every time a branch retires. Updates statistics and transitions the counter state.

- Increments `total_branches` always.
- Increments `correct_predictions` if `was_correct` is true.
- Uses `int& state = counter[pc]` — the `&` makes this a reference, so writing to `state` directly modifies the map entry.

State transitions on taken: 1→0, 2→1, 3→2 (state 0 stays 0).
State transitions on not-taken: 0→1, 1→2, 2→3 (state 3 stays 3).

---

## 4. ExecutionUnit.h — All Functions

---

### `struct InFlightEntry`

Represents one instruction currently working its way through the execution pipeline.


| Field              | Meaning                                                             |
| ------------------ | ------------------------------------------------------------------- |
| `rob_tag`          | Which ROB slot this instruction belongs to                          |
| `result`           | The pre-computed result (computed at issue time, not after latency) |
| `exception`        | Whether this instruction faulted                                    |
| `cycles_remaining` | How many more cycles until this instruction is done                 |

Note that the result is computed immediately when the instruction issues from the RS. The `cycles_remaining` countdown is purely to model the hardware latency — it doesn't affect when the computation happens in simulation.

---

### `ExecutionUnit(UnitType t, int lat, int rs_size)` — constructor

Sets `name` and `latency`, then resizes the `rs` vector to `rs_size` slots, all initially invalid.

---

### `capture(int tag, int val)`

```cpp
void capture(int tag, int val)
```

Called during CDB broadcast. Scans every valid RS entry. If an entry is waiting for operand 1 (`qj == tag` and `vj_ready == false`), it fills in `vj = val` and sets `vj_ready = true` and `qj = -1`. Does the same for operand 2 (`qk`/`vk`). A single broadcast can unblock multiple waiting RS entries simultaneously.

---

### `allocate(const RSEntry& e)`

```cpp
int allocate(const RSEntry& e)
```

Finds the first invalid RS slot, copies `e` into it, sets it valid, and returns the slot index. Returns -1 if all slots are occupied. Called during the Decode stage when issuing an instruction to this unit.

---

### `isFull()`

```cpp
bool isFull() const
```

Returns true if every RS slot is valid (occupied). Checked during Decode to decide whether to stall.

---

### `findOldestReady()`

```cpp
int findOldestReady() const
```

Scans all valid RS entries and finds the one with the smallest `issue_cycle` where both `vj_ready` and `vk_ready` are true. Returns that entry's index, or -1 if no ready entry exists. The oldest-first selection ensures instructions execute in a fair, age-based order, which helps avoid starvation.

---

### `compute(const RSEntry& e, bool& exc)` — static

```cpp
static int compute(const RSEntry& e, bool& exc)
```

Evaluates the instruction using the already-resolved operand values `vj` and `vk` from the RS entry. Sets `exc = true` and returns a garbage value (to be ignored) if an exception condition is detected. The `exc` parameter is passed by reference so the result can be written back to the caller.

Exception conditions:

- ADD, SUB, ADDI, MUL, DIV, REM: promotes to `long long` and checks if result is outside `[-2^31, 2^31-1]`
- DIV, REM: also raises exception if `vk == 0` (division by zero)
- AND, OR, XOR, ANDI, ORI, XORI, SLT, SLTI: never raise exceptions

For branches, returns 1 if the branch condition is true (taken), 0 if false (not-taken). This value is later interpreted by `broadcastOnCDB` to set `actual_taken` in the ROB.

---

### `executeCycle(int current_cycle)`

```cpp
void executeCycle(int current_cycle)
```

The main per-cycle function. Called once per clock cycle by `stageExecuteAndBroadcast`. Does three things in order:

**Step 1 — Reset result flags**
Clears `has_result`, `has_exception`, `result_tag`, `result_val` to false/zero. These are output flags that only stay set for one cycle.

**Step 2 — Advance pipeline**
Decrements `cycles_remaining` on every in-flight entry in the `pipeline` deque.

**Step 3 — Check for completion**
If the front entry of the deque has `cycles_remaining <= 0`, it is done. Its `rob_tag`, `result`, and `exception` are copied into the unit's output fields (`has_result = true`, `result_tag`, `result_val`, `has_exception`), and it is removed from the deque with `pop_front`.

**Step 4 — Issue a new instruction**
Calls `findOldestReady` to find the best RS candidate. If one is found, `compute` is called immediately to get the result, a new `InFlightEntry` is created with `cycles_remaining = latency`, and it is pushed to the back of the deque. The RS slot is freed immediately (`e.valid = false`) so another instruction can occupy it in the next cycle.

This design allows true pipeline overlap: a new instruction starts every cycle that one is ready, while older ones are still counting down their latency in the front of the deque.

---

## 5. LoadStoreQueue.h — All Functions

The LSQ is fundamentally different from other execution units: it only issues instructions in program order (oldest first). This is because memory addresses are not renamed, so a load and a store to the same address must execute in the correct sequence.

---

### `struct LSQEntry`

Similar to `RSEntry` but for memory operations.


| Field         | Meaning                                                                             |
| ------------- | ----------------------------------------------------------------------------------- |
| `vj` / `qj`   | Base register value / producing ROB tag (for address calculation:`addr = vj + imm`) |
| `vk` / `qk`   | Store data value / producing ROB tag (only used for SW)                             |
| `imm`         | Memory offset (label base address + any numeric offset)                             |
| `dest`        | Destination register for LW (-1 for SW)                                             |
| `issue_cycle` | Used to find the oldest entry                                                       |

---

### `struct StoreBufferEntry`

A store that has finished executing but whose ROB entry has not yet committed. Exists so loads can see store values before they are written to `Memory[]`.


| Field         | Meaning                                                                      |
| ------------- | ---------------------------------------------------------------------------- |
| `rob_tag`     | Identifies which store this is (used for cleanup at commit)                  |
| `addr`        | The computed memory address                                                  |
| `val`         | The value to be stored                                                       |
| `issue_cycle` | Used to pick the most recent store when multiple stores hit the same address |

---

### `struct LSQInFlight`

Tracks one in-flight load or store inside the LSQ pipeline. Same role as `InFlightEntry` in `ExecutionUnit`.

---

### `LoadStoreQueue(int lat, int rs_size)` — constructor

Sets latency, sizes the `rs` vector to `rs_size`, and sizes `store_buffer` to `rs_size * 2` (generous to prevent it filling up).

---

### `isFull()`

```cpp
bool isFull() const
```

Returns true if every LSQ RS slot is occupied. Checked in Decode to decide whether to stall.

---

### `allocate(const LSQEntry& e)`

```cpp
int allocate(const LSQEntry& e)
```

Finds the first free RS slot, copies `e` into it, marks it valid, returns the index. Returns -1 if full.

---

### `capture(int tag, int val)`

```cpp
void capture(int tag, int val)
```

Same as `ExecutionUnit::capture` but operates on `LSQEntry` items in the RS. Fills in `vj` if `qj == tag` (base register becomes available) or `vk` if `qk == tag` (store data becomes available), then marks the operand ready.

---

### `findOldest()`

```cpp
int findOldest() const
```

Finds the RS entry with the smallest `issue_cycle` (the oldest instruction in the queue). Unlike `ExecutionUnit::findOldestReady`, it does **not** check whether operands are ready — that check happens separately in `executeCycle`. This is because the LSQ must always try to issue the oldest entry first; it cannot skip over a not-ready entry to issue a newer one.

---

### `executeCycle(std::vector<int>& Memory)`

```cpp
void executeCycle(std::vector<int>& Memory)
```

The main per-cycle function. Steps:

**Step 1 — Reset result flags**
Clears all output flags.

**Step 2 — Advance pipeline**
Decrements `cycles_remaining` on all in-flight entries.

**Step 3 — Check for completion**
If the front entry is done, copies its fields into the output result fields and pops it. For loads, `result_val` holds the loaded data. For stores, `result_is_store`, `result_store_addr`, `result_store_val` are set so `broadcastOnCDB` can copy them into the ROB.

**Step 4 — Try to issue the oldest entry**
Calls `findOldest`. If no entry exists, returns. Then checks:

- If `vj` (base register) is not ready: return, cannot compute address yet.
- If it is a SW and `vk` (store data) is not ready: return.
- If it is a LW and there is any in-flight store in the pipeline deque: return (stall the load to avoid reading stale memory before the store finishes).

**Step 5 — Execute**
Computes `addr = vj + imm`. Checks bounds; sets `exc = true` if out of range.

For LW:

- Checks the store buffer for any entry with a matching address. If found, uses the most recently issued matching store's value (store-to-load forwarding) instead of reading `Memory[addr]`. This is how a load sees a store's value before it commits.
- Also checks the pipeline for any in-flight store to the same address as a secondary forwarding path.

For SW:

- Records `store_addr` and `store_val` in the `InFlightEntry`.
- Immediately adds an entry to the store buffer so future loads can forward from it.

Frees the RS slot and pushes the new `LSQInFlight` onto the pipeline deque.

---

### `commitStore(int rob_tag)`

```cpp
void commitStore(int rob_tag)
```

Called from `Processor::stageCommit` when a store's ROB entry retires. Searches the store buffer and marks the matching entry invalid. This removes the store from the forwarding buffer now that it has been officially written to `Memory[]`.

---

### `flush()`

```cpp
void flush()
```

Clears all RS entries, all store buffer entries, and the pipeline deque. Called after branch mispredictions and exceptions to discard all speculative load/store operations.

---

## 6. Processor.h — All Functions

---

### `Processor(ProcessorConfig& config)` — constructor

Initialises all hardware:

- Sets `pc = 0`, `clock_cycle = 0`
- Resizes `ARF` to `num_regs` (all zero), `Memory` to `mem_size` (all zero), `RAT` to `num_regs` (all -1, meaning every register is up-to-date in ARF), `ROB` to `rob_size`
- Creates 5 execution units with `emplace_back`: Adder (index 0), Multiplier (1), Divider (2), Branch (3), Logic (4)
- Creates the LSQ with `new LoadStoreQueue(mem_lat, lsq_rs_size)`

---

### `loadProgram(const std::string& filename)`

```cpp
void loadProgram(const std::string& filename)
```

Reads and parses the assembly source file. Runs two passes.

**Pass 1** reads every line, strips comments (everything after `#`), and trims whitespace. Classifies each non-empty line:

- Lines starting with `.` are memory declarations (`.A: 1 2 3`). Values are written into `Memory` starting at `mem_offset`, which advances after each value. The label name and its starting memory index are saved in `mem_labels`.
- Lines where a word ending in `:` appears before any space are instruction labels. The label name is mapped to the current instruction count in `labels`. Any remaining text on the same line is treated as an instruction.
- All other lines are instructions. Each is saved with its index in `inst_lines`.

**Pass 2** calls `parseInstruction` for each saved instruction line and stores the result in `inst_memory[idx]`.

---

### `parseInstruction(line, pc_val, labels, mem_labels)`

```cpp
Instruction parseInstruction(const std::string& line, int pc_val,
    std::unordered_map<std::string,int>& labels,
    std::unordered_map<std::string,int>& mem_labels)
```

Converts one line of assembly text into an `Instruction` struct.

Uses two lambda helpers defined inside the function:

**`stripComma(string& s)`** — removes a trailing comma if present. Needed because operands in the assembly file are comma-separated.

**`parseReg(const string& s)`** — converts a register name like `"x5"` to the integer 5. Strips any trailing comma first.

**`resolveImm(const string& s, int cur_pc)`** — converts an immediate token to an integer:

- If it starts with a digit, `-`, or `+`: parse as integer directly.
- If it matches a key in `labels`: compute PC-relative offset as `labels[s] - cur_pc`. This is why branch instructions pass `pc_val` and arithmetic instructions pass `0`.
- If it matches a key in `mem_labels`: return the absolute memory index.

After reading the opcode string, dispatches to the appropriate parsing block for each instruction type. Arithmetic and logic instructions read three tokens (dest, src1, src2 or imm). Memory instructions use `parseMem`. Branch instructions read two register tokens and a label/offset.

---

### `parseMem(memop, base_reg, offset, labels, mem_labels)`

```cpp
void parseMem(const std::string& memop, int& base_reg, int& offset, ...)
```

Parses a memory operand string of the form `"A(x1)"` or `"4(x2)"` into its two components.

Finds the `(` and `)` characters to split the string. Everything before `(` is the offset/label; everything between `(` and `)` is the base register name. Resolves the offset the same way as `resolveImm` (numeric or label lookup). Parses the base register with `stoi`. `base_reg` and `offset` are written by reference so both values come back to the caller.

---

### `robFull()`

```cpp
bool robFull() const
```

Returns true if `rob_count == ROB.size()`. Checked in Decode before allocating a new ROB entry.

---

### `robAllocate(const Instruction& inst, int predicted_pc)`

```cpp
int robAllocate(const Instruction& inst, int predicted_pc)
```

Allocates the next free ROB slot (at `rob_tail`), fills it in, advances `rob_tail` circularly, increments `rob_count`, and returns the tag (the slot index). The tag is what gets stored in `RAT[dest]` and in `RSEntry::rob_tag` so results can be associated with this instruction later.

Sets:

- `valid = true`, `ready = false`, `has_exception = false`
- `dest`, `pc`, `op` from the instruction
- `is_branch` — true for BEQ, BNE, BLT, BLE, J
- `branch_imm` — the immediate, saved so `broadcastOnCDB` can compute the actual branch target
- `is_store` — true for SW
- `predicted_taken` — from `bp.predictsTaken(inst.pc)`, or always true for J
- `predicted_target` — the PC that fetch already jumped to (passed in as `predicted_pc`)

---

### `resolveOperand(int reg, bool& ready, int& val, int& tag)`

```cpp
void resolveOperand(int reg, bool& ready, int& val, int& tag)
```

Looks up the current value of a source register at decode time, considering in-flight instructions. All four outputs are written by reference.

Logic:

1. If `reg < 0`: operand is unused, mark ready with value 0.
2. If `RAT[reg] == -1`: no in-flight instruction is writing this register, so read from `ARF[reg]`. Mark ready.
3. If `RAT[reg]` points to a ROB entry that is already `ready`: forward the computed value directly from `ROB[rat_tag].value`. Mark ready.
4. Otherwise: the value is not available yet. Set `ready = false` and `tag = rat_tag` so the RS entry records which ROB tag to watch for on the CDB.

---

### `unitIndex(UnitType t)`

```cpp
int unitIndex(UnitType t) const
```

Converts a `UnitType` enum to the corresponding index in the `units` vector: ADDER=0, MULTIPLIER=1, DIVIDER=2, BRANCH=3, LOGIC=4. Used whenever the processor needs to look up a specific unit by type.

---

### `getUnitType(OpCode op)`

```cpp
UnitType getUnitType(OpCode op)
```

Returns which execution unit an instruction belongs to based on its opcode. Used in Decode to decide where to send the instruction.


| Opcode(s)                     | Unit       |
| ----------------------------- | ---------- |
| ADD, SUB, ADDI, SLT, SLTI     | ADDER      |
| MUL                           | MULTIPLIER |
| DIV, REM                      | DIVIDER    |
| BEQ, BNE, BLT, BLE            | BRANCH     |
| AND, OR, XOR, ANDI, ORI, XORI | LOGIC      |
| LW, SW                        | LOADSTORE  |

---

### `flush()`

```cpp
void flush()
```

Discards all speculative pipeline state while preserving committed state (`ARF` and `Memory` are untouched). Called after branch mispredictions and exceptions.

What it clears:

- `fd_reg.valid = false` — instruction in the fetch/decode pipeline register is discarded
- `di_reg.valid = false`, `di_reg.stall = false` — instruction in the decode/issue pipeline register is discarded
- All ROB entries: sets every `valid = false`, resets `rob_head`, `rob_tail`, `rob_count` to 0
- All RAT entries: sets every entry to -1 (all registers now authoritative in ARF)
- All RS entries in every execution unit: sets every `valid = false`, clears the pipeline deque
- Entire LSQ: calls `lsq->flush()` which clears RS, store buffer, and pipeline
- `fetch_done = false` — re-enables fetch

---

### `findImmForROB(const ROBEntry& re)`

```cpp
int findImmForROB(const ROBEntry& re)
```

A simple accessor that returns `re.branch_imm`. Used by `broadcastOnCDB` when computing the actual branch target from the ROB entry. Exists as a named function for clarity.

---

### `broadcastOnCDB()`

```cpp
void broadcastOnCDB()
```

Called after all execution units have run for the cycle. Collects every result produced this cycle and distributes it to all waiting reservation stations and the ROB.

**Step 1 — Collect results**
Builds a local `results` vector. For each execution unit, if `has_result` is true, adds a `CDBResult` entry with `tag`, `val`, `exc`. Does the same for the LSQ, also capturing `is_store`, `store_addr`, `store_val`.

**Step 2 — Update the ROB**
For each result, finds the matching ROB entry by tag. Sets `ready = true` and `has_exception`. Then:

- For stores: copies `store_addr` and `store_val` into the ROB entry so commit can write memory.
- For branches: interprets `val` as a taken/not-taken flag (1=taken, 0=not-taken). Sets `actual_taken` and computes `actual_target` as `pc + branch_imm` if taken or `pc + 1` if not-taken.
- For all others: stores `val` as the result.

**Step 3 — Forward to reservation stations**
For each result, calls `capture(tag, val)` on every execution unit and on the LSQ. This unblocks any RS entries that were waiting for this result.

---

### `stageFetch()`

```cpp
void stageFetch()
```

Fetches one instruction per cycle into `fd_reg`.

**Stall conditions:**

- If `fetch_done` is true: do nothing.
- If `di_reg.valid` is true (decode stage is holding an instruction, stalled or not): do nothing. Fetch only runs when the decode stage is free.

**Normal operation:**

- If `pc >= inst_memory.size()`: sets `fetch_done = true` and returns.
- Reads `inst_memory[pc]` into a local `inst`.
- Computes `next_pc`:
  - Unconditional jump: `pc + imm` (always taken, no prediction needed)
  - Conditional branch: calls `bp.predict(pc, imm, op)` which returns either `pc + imm` or `pc + 1` based on the 2-bit counter state
  - All others: `pc + 1`
- Writes `inst` into `fd_reg`, sets `fd_reg.valid = true`.
- Advances `pc` to `next_pc`.

---

### `stageDecode()`

```cpp
void stageDecode()
```

Moves an instruction from `fd_reg` into `di_reg` and attempts to issue it to the appropriate execution unit.

**Move from fd to di:**
If `di_reg` is empty or was stalled, and `fd_reg` has an instruction, the instruction is moved over. `fd_reg` is cleared.

**Unconditional jump (J):**
Handled entirely here without touching any execution unit. A ROB entry is allocated and immediately marked `ready = true`, `actual_taken = true`, `actual_target = inst.pc + imm`. No RS entry is created.

**Stall checks:**
If the ROB is full, or the target unit's RS is full, `di_reg.stall = true` is set and the function returns without issuing anything. The instruction waits in `di_reg` until resources free up.

**Operand resolution:**
Calls `resolveOperand` for `src1` and `src2`. For each, either gets the ready value from ARF or a forwarded ROB value, or records the waiting ROB tag.

**RAT update:**
If `inst.dest > 0` (not x0), sets `RAT[inst.dest] = rob_tag`. This tells future instructions that look up this register to watch for this ROB tag on the CDB instead of reading ARF.

**Issue:**
Fills an `RSEntry` (or `LSQEntry`) with all the resolved operand info and calls `units[unitIndex(ut)].allocate(e)` or `lsq->allocate(e)`. Clears `di_reg`.

---

### `stageExecuteAndBroadcast()`

```cpp
void stageExecuteAndBroadcast()
```

Runs one cycle of every execution unit and the LSQ, then broadcasts all results.

Calls `executeCycle` on all 5 units and `lsq->executeCycle(Memory)`, then calls `broadcastOnCDB()`. The broadcast happens in the same cycle that results are produced, so waiting RS entries can potentially issue in the very next cycle.

---

### `stageCommit()`

```cpp
void stageCommit()
```

Retires the instruction at the head of the ROB if it is ready. Only one instruction commits per cycle.

**Guard:** if `rob_count == 0` or the head entry is not ready, do nothing.

**Exception:**
If `head.has_exception`: sets `Processor::exception = true`, sets `pc = head.pc` (the faulting instruction's PC), calls `flush()`, sets `halted = true`. Execution stops.

**Branch:**
Checks whether the prediction was correct: `predicted_taken == actual_taken` and the predicted target matches the actual target (with special handling for not-taken branches where the target is always `pc + 1`).

Calls `bp.update` regardless of correctness.

If mispredicted: sets `pc` to the correct next instruction, retires the branch's ROB entry (advances `rob_head`, decrements `rob_count`), calls `flush()` to wipe all speculative state, and returns.

**Store:**
Writes `Memory[head.store_addr] = head.store_val`. Calls `lsq->commitStore(rob_head)` to remove it from the store buffer.

**Register write:**
If `head.dest > 0`, writes `ARF[head.dest] = head.value`. Clears `RAT[head.dest]` back to -1 only if it still points to this ROB entry — a later instruction writing the same register may have updated the RAT since. Then forces `ARF[0] = 0` (x0 is always zero).

Finally advances `rob_head` circularly and decrements `rob_count`.

---

### `step()`

```cpp
bool step()
```

Executes exactly one clock cycle. Called repeatedly by `main` until it returns false.

Increments `clock_cycle`. Calls the four stages in **reverse order**: Commit → Execute/Broadcast → Decode → Fetch. This reverse ordering ensures that a commit in cycle N cannot allow the freed ROB slot to be immediately reused by decode in the same cycle N.

Returns `false` (halting) when:

- `halted` is true (exception occurred), OR
- `fetch_done` is true AND `fd_reg`, `di_reg` are both empty AND `rob_count == 0` AND all unit pipeline deques are empty

Returns `true` while there is still work to do.

---

### `dumpArchitecturalState()`

```cpp
void dumpArchitecturalState()
```

Prints the contents of `ARF`, the exception status, and branch predictor statistics. Called by `main` after execution completes.

---

## 7. Full Workflow: What Happens Each Cycle

Every call to `step()` runs this sequence:

```
1. COMMIT
   └─ Look at ROB head
   └─ If ready: check exception → check branch → write memory/registers → advance head

2. EXECUTE + BROADCAST
   └─ Each ExecutionUnit::executeCycle()
      └─ Decrement cycles_remaining on all in-flight entries
      └─ Pop front entry if done, set has_result
      └─ Issue oldest-ready RS entry into pipeline
   └─ LoadStoreQueue::executeCycle()
      └─ Same advance/complete/issue logic, but in-order only
   └─ broadcastOnCDB()
      └─ Collect all has_result flags
      └─ Update ROB entries (ready, value, exception, branch fields, store fields)
      └─ Call capture() on all units and LSQ to forward values to waiting RS entries

3. DECODE
   └─ Move fd_reg → di_reg if di_reg is free
   └─ Try to issue di_reg instruction:
      └─ Check ROB full → stall if so
      └─ Check RS full → stall if so
      └─ Resolve operands via RAT
      └─ Allocate ROB entry
      └─ Update RAT
      └─ Allocate RS/LSQ entry

4. FETCH
   └─ Stall if di_reg is occupied
   └─ Read inst_memory[pc]
   └─ Predict next PC via BranchPredictor
   └─ Write to fd_reg
   └─ Advance pc
```

---

## 8. Full Workflow: What Happens at Program Start

```
1. Processor constructor runs:
   - ARF filled with zeros
   - Memory filled with zeros
   - RAT filled with -1 (all registers authoritative in ARF)
   - ROB slots all marked invalid
   - 5 ExecutionUnits created with their RS sizes and latencies
   - LSQ created

2. loadProgram() runs:
   - Pass 1: memory labels parsed → Memory[] populated, mem_labels map built
             instruction labels parsed → labels map built
             instruction lines collected with their PC indices
   - Pass 2: each instruction line parsed → inst_memory[] filled

3. main() calls step() in a loop:
   - First cycle: Fetch reads inst_memory[0], advances pc to 1 (or predicted target)
   - Second cycle: Decode issues the first instruction, Fetch reads inst_memory[1]
   - Execution units start firing as soon as operands are ready
   - Commit retires instructions in order as their ROB entries become ready
   - Loop ends when step() returns false
```

---

## 9. Full Workflow: Branch Misprediction

```
Cycle F:   Fetch reads branch at PC=5, predictor says taken → pc jumps to PC=3
Cycle F+1: Fetch reads PC=3 (speculative)
...
           (several speculative instructions are fetched, decoded, issued)
...
Cycle E:   Branch execution unit completes, result=0 (not taken)
           broadcastOnCDB sets ROB[branch_tag].actual_taken = false
                                ROB[branch_tag].actual_target = 6  (pc+1)
                                ROB[branch_tag].ready = true
...
Cycle C:   Branch reaches ROB head, stageCommit runs:
           - predicted_taken=true, actual_taken=false → MISPREDICTION
           - correct_next = branch_pc + 1 = 6
           - bp.update(5, 6, false, false) → predictor state changes
           - branch ROB entry is retired (rob_head advances)
           - flush() is called:
             * fd_reg cleared
             * di_reg cleared
             * remaining ROB entries invalidated
             * RAT reset to all -1
             * all RS entries cleared
             * all unit pipelines cleared
             * LSQ cleared
           - pc = 6
Cycle C+1: Fetch restarts from PC=6 (correct path)
```

---

## 10. Full Workflow: Exception

```
Cycle E:   Divider unit completes div x1, x2, x3 where x3=0
           compute() sets exc=true, returns 0
           InFlightEntry.exception = true pushed into pipeline

Cycle E+lat: Entry reaches front of divider pipeline
             has_exception = true, result_tag = rob_tag_of_div
             broadcastOnCDB runs:
             - ROB[rob_tag].ready = true
             - ROB[rob_tag].has_exception = true
             - All RS entries still get the result value (even though it's garbage)
               so the pipeline keeps moving — the exception only matters at commit

...
           (instructions after the div may still execute speculatively)
...

Cycle C:   Division instruction reaches ROB head, stageCommit runs:
           - head.has_exception == true
           - exception = true  (processor's exception flag set)
           - pc = head.pc  (PC of the faulting instruction)
           - flush()  (all speculative state wiped)
           - halted = true

Cycle C+1: step() returns false immediately because halted==true
           main() prints exception message and dumps architectural state
           ARF reflects all instructions that committed before the division
           Memory reflects all stores that committed before the division
```

---

## 11. Data Flow Diagram

```
 Assembly file
      |
      v
 loadProgram()
      |
      v
 inst_memory[]
      |
      v
+--[ FETCH ]------------------------------------------+
| Read inst_memory[pc]                                 |
| Predict next PC via BranchPredictor                  |
| Write to fd_reg, advance pc                          |
+------------------------------------------------------+
      |
      v  fd_reg
+--[ DECODE ]------------------------------------------+
| Read fd_reg, write to di_reg                         |
| Check ROB full / RS full → stall                     |
| resolveOperand() → read ARF or forward from ROB      |
| robAllocate() → get ROB tag                          |
| Update RAT[dest] = rob_tag                           |
| ExecutionUnit::allocate() / lsq->allocate()          |
+------------------------------------------------------+
      |
      v  RS entries / LSQ entries (with vj,vk or qj,qk)
+--[ EXECUTE ]-----------------------------------------+
| All ExecutionUnit::executeCycle()                    |
|   - advance pipeline countdowns                      |
|   - complete front-of-pipeline instruction           |
|   - issue oldest-ready RS entry                      |
| LoadStoreQueue::executeCycle()                       |
|   - advance pipeline                                 |
|   - issue oldest entry (in-order, with stall rules)  |
|   - store-to-load forwarding via store buffer        |
+------------------------------------------------------+
      |
      v  has_result, result_tag, result_val, has_exception
+--[ BROADCAST (CDB) ]---------------------------------+
| broadcastOnCDB()                                     |
|   - update ROB entry: ready=true, value, exception   |
|   - for branches: set actual_taken, actual_target    |
|   - for stores: set store_addr, store_val            |
|   - call capture() on all RS / LSQ                   |
|     → unblock waiting operands                       |
+------------------------------------------------------+
      |
      v  ROB entries marked ready
+--[ COMMIT ]------------------------------------------+
| Check ROB head: valid and ready?                     |
| Exception?   → set pc, flush, halt                   |
| Branch?      → check prediction                      |
|                bp.update()                           |
|                misprediction? → flush, redirect pc   |
| Store?       → Memory[addr] = val                    |
|                lsq->commitStore()                    |
| Register?    → ARF[dest] = value                     |
|                clear RAT[dest] if still pointing here|
| Advance rob_head                                     |
+------------------------------------------------------+
      |
      v
  ARF / Memory (committed architectural state)
```


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

  ### std::unordered_map<int,int> counter is a map or dictionary which maps each pc value or each instruction to a counter (0,1,2,3).
- int &state= counter[pc] creates a new map entry for the pc if pc not already in map, is mapped to 0. &state is used so that the value attached to the pc can be directly changed in the map.
- The get_state function is basically the same as  return counter[pc]. It would also check if the pc is mapped to something and if not, map it to 0.

---

## ExecutionUnit.h

### Structure

InFlightEntry: The instructions belonging to this excecution unit whose all sources were available and is now in process of being calculated.

Each `ExecutionUnit` owns:

- A `std::vector<RSEntry> rs` — the reservation station for this unit
- A `std::deque<InFlightEntry> pipeline` — the in-flight pipeline stages
- Result fields (`has_result`, `result_tag`, `result_val`, `has_exception`) that are set for exactly one cycle when an instruction completes. Every cycle only one result per execution unit can be produced. These are the values of that completed instruction.

### `executeCycle(current_cycle)`

Called once per clock cycle. It does three things in order:

1. **Advance the pipeline** — decrements `cycles_remaining` on every in-flight entry.
2. **Check for completion** — if the front entry's `cycles_remaining` reaches 0, it is popped and its result fields are set. Only one result is produced per cycle (the front of the deque).
3. **Issue a new instruction** — calls `findOldestReady()` to find the RS entry with the smallest `issue_cycle` where both operands are ready. That entry is removed from the RS immediately and pushed onto the back of the pipeline with `cycles_remaining = latency`. This means a new instruction starts every cycle as long as something is ready, giving true pipeline overlap.

### `compute(entry, exc)`

A static helper that evaluates the arithmetic or logical result from the RS entry's operand values and opcode. For operations that can overflow (ADD, SUB, ADDI, MUL, DIV, REM) it promotes to `long long` and sets `exc = true` if the result falls outside the 32-bit signed range. Division and remainder by zero also set `exc`. Logic operations (AND, OR, XOR, etc.) never raise exceptions. Branch instructions return 1 (taken) or 0 (not-taken) as their result — the actual target is computed later in `broadcastOnCDB`.

### `capture(tag, val)`

Called during CDB broadcast. Scans the RS and fills in `vj` or `vk` for any entry whose `qj` or `qk` matches the given tag, then marks the operand ready.

### allocate(& RSentry e)

It allocates a place to a new instruction in the reservation station.

### findOldestReady()

It checks the reservation station and finds the entry whose both sources are ready and is oldest from the instruction set.

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
