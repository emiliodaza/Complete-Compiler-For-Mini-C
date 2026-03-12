#include <iostream>
#include <llvm-c/Core.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cstudio>

// returns instructions mapped to registers or -1 if to the point of the final result they 
// spilled
std::unordered_map<LLVMValueRef, int> register_allocation_algorithm(char* filename) {
    std::unordered_map<LLVMValueRef, int> reg_map;

    LLVMContextRef context = LLVMContextCreate();
    LLVMMemoryBufferRef buffer = NULL;
    LLVMModuleRef module = NULL;
    char* err = NULL;

    // reading the .ll file into the buffer
    if (LLVMCreateMemoryBufferWithContentsOfFile(filename, &buffer, &err)) {
        fprintf(stderr, "%s", err);
        LLVMDisposeMessage(err);
        exit(1);
    }
    // parse the IR text into the module
    if (LLVMParseIRInContext(context, buffer, &module, &err)) {
        fprintf(stderr, "%s", err);
        LLVMDisposeMessage(err);
        exit(2);
    }

    for (LLVMValueRef func = LLVMGetFirstFunction(module); func != NULL; func = LLVMGetNextFunction(func)){
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)){
            std::unordered_set<int> available = {0,1,2}; // 0 refers to ebx, 1 refers to ecx, and 2 refers to edx
            std::unordered_map<LLVMValueRef, int> inst_index; // map of instructions to their respective index
            int index_counter = 0;
            for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
                // ignoring allocas
                if (LLVMGetInstructionOpcode(ins) == LLVMAlloca) {
                    continue;
                }
                inst_index[ins] = index_counter;
                index_counter++;
            } 
            std::unordered_map <LLVMValueRef, std::pair<int,int>> live_range = compute_liveness(bb, inst_index);
            for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
                // ignoring alloca
                if (LLVMGetInstructionOpcode(ins) == LLVMAlloca) {
                    continue;
                }
                // handling case when ins does not have a result
                else if (LLVMGetTypeKind(LLVMTypeOf(ins)) == LLVMVoidTypeKind) {
                    int num_operands = LLVMGetNumOperands(ins);
                    // inspecting operands
                    for (int operand_index = 0; operand_index < num_operands; operand_index++){
                        LLVMValueRef operand = LLVMGetOperand(ins, operand_index);
                        if (live_range.find(operand) != live_range.end() && live_range[operand].second == inst_index[ins]) { // if the live range of the operand ends here
                            // if it has a register assigned to it then we add that register to available
                            if (reg_map.find(operand) != reg_map.end() && reg_map[operand] != -1){
                                available.insert(reg_map[operand]);
                            }
                        }
                    }
                }
                else {
                    // third case of instruction
                    // condition if the instruction is of type add/sub/mul
                    LLVMOpcode opcode = LLVMGetInstructionOpcode(ins);
                    bool operation_condition = opcode == LLVMAdd || opcode == LLVMSub || opcode == LLVMMul;
                    // condition if the first operand has a physical register assigned to it in reg_map
                    bool first_operand_register_condition = false;
                    // condition if the liveness range of the first operand ends at the current instruction
                    bool first_operand_ends_curr_ins = false;
                    if (LLVMGetNumOperands(ins) > 0){
                        LLVMValueRef first_operand = LLVMGetOperand(ins, 0);
                        if (reg_map.find(first_operand) != reg_map.end() && reg_map[first_operand] != -1){
                            first_operand_register_condition = true;
                        }
                        if (live_range.find(first_operand) != live_range.end() && live_range[first_operand].second == inst_index[ins]){ // liveness range ends at the curr instruction
                            first_operand_ends_curr_ins = true;
                        }
                    }
                    // if the three conditions are true then we add the entry ins -> R to reg_map
                    if (operation_condition && first_operand_register_condition && first_operand_ends_curr_ins){
                        LLVMValueRef first_operand = LLVMGetOperand(ins, 0);
                        reg_map[ins] = reg_map[first_operand];
                        if (LLVMGetNumOperands(ins) > 1){
                            LLVMValueRef second_operand = LLVMGetOperand(ins, 1);
                            // if live range of the second operand ends and it has a physical register then we add it to available
                            if (live_range.find(second_operand) != live_range.end() && live_range[second_operand].second == inst_index[ins]){
                                if (reg_map.find(second_operand) != reg_map.end() && reg_map[second_operand] != -1){
                                    available.insert(reg_map[second_operand]);
                                }
                            } 
                        }
                    }
                    else if (!available.empty()){
                        // if there is a register available it becomes R
                        int R = *available.begin();
                        // adding the entry ins -> R to reg_map
                        reg_map[ins] = R;
                        // removing R from available
                        available.erase(R);
                        // if live range of any operand of ins ends and it has a physical register P then add it to available
                        int num_operands = LLVMGetNumOperands(ins);
                        for (int operand_index = 0; operand_index < num_operands; operand_index++){
                            LLVMValueRef curr_operand = LLVMGetOperand(ins, operand_index);
                            if (live_range.find(curr_operand) != live_range.end() && live_range[curr_operand].second == inst_index[ins]){
                                if (reg_map.find(curr_operand)!= reg_map.end() && reg_map[curr_operand] != -1){
                                    int P = reg_map[curr_operand];
                                    available.insert(P);
                                }
                            }
                        }
                    } 
                    else {
                        // creating sorted_list of instructions based on the first heuristic: by the endpoints in live_map (decreasing order)
                        std::vector<LLVMValueRef> sorted_list;
                        // getting all key-value pairs by reference
                        for (auto& ins_pair : live_range){
                            sorted_list.push_back(ins_pair.first); // the key is added: the instruction
                        }
                        // sorting it
                        std::sort(sorted_list.begin(), sorted_list.end(), [&](LLVMValueRef a, LLVMValueRef b){ // sorts in plce by determining if a should come before b given a specified condition
                            return live_range[a].second > live_range[b].second; // if we have that an instructions endpoint is greater than another takes priority
                        });
                        // using find_spill to find the LLVMValueRef V to spill based on heuristic
                        LLVMValueRef V = find_spill(ins, reg_map, inst_index, sorted_list, live_range);
                        if (live_range.find(V) != live_range.end() && live_range.find(ins) != live_range.end()){
                            int end_point_of_V = live_range[V].second;
                            int end_point_of_ins = live_range[ins].second;
                            // if liveness range of ins ends after liveness range of V
                            if (end_point_of_ins > end_point_of_V){
                                reg_map[ins] = -1;
                            } else {
                                if (reg_map.find(V) != reg_map.end()){
                                    // getting physical register assigned to V
                                    int R = reg_map[V];
                                    // adding entry to reg_map
                                    reg_map[ins] = R;
                                    // updating the entry for V from V -> R to V -> -1 in reg_map
                                    reg_map[V] = -1;
                                }
                            }
                            int num_operands = LLVMGetNumOperands(ins);
                            for (int operand_index = 0; operand_index < num_operands; operand_index++){
                                LLVMValueRef curr_operand = LLVMGetOperand(ins, operand_index);
                                // checking if the live range of the current operand ends in this instruction
                                if (live_range.find(curr_operand) != live_range.end() && live_range[curr_operand].second == inst_index[ins]){
                                    // checking if the operand has a physical register P assigned to it, and if so add P to available
                                    if (reg_map.find(curr_operand) != reg_map.end() && reg_map[curr_operand] != -1){
                                        int P = reg_map[curr_operand];
                                        available.insert(P);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

std::unordered_map <LLVMValueRef, std::pair<int,int>> compute_liveness(LLVMBasicBlockRef bb, std::unordered_map<LLVMValueRef, int> inst_index){
    std::unordered_map <LLVMValueRef, std::pair<int,int>> live_range;

    // initialize the first and second element in the pair by the start index for now
    for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
        // skipping allocas
        if (LLVMGetInstructionOpcode(ins) == LLVMAlloca) {continue;}
        // skipping instructions that do not generate a value
        if (LLVMGetTypeKind(LLVMTypeOf(ins)) == LLVMVoidTypeKind) {continue;}
        // we have now valid instructions that hold a value for live_range
        live_range[ins] = {inst_index[ins], inst_index[ins]}; // the instruction where the value is declared is considered as key
    }

    // updating the second element in the pair
    for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
        int num_operands = LLVMGetNumOperands(ins);
        for (int operand_index = 0; operand_index < num_operands; operand_index++){
            LLVMValueRef operand = LLVMGetOperand(ins, operand_index);
            if (inst_index.find(operand) != inst_index.end()){
                live_range[operand].second = inst_index[ins]; // we update the second if we encountered a instruction that used the operand 
            }
        }
    }
    return live_range;
}

LLVMValueRef find_spill(LLVMValueRef ins, std::unordered_map<LLVMValueRef, int>& reg_map, std::unordered_map<LLVMValueRef, int>& inst_index, std::vector<LLVMValueRef>& sorted_list, std::unordered_map <LLVMValueRef, std::pair<int,int>>& live_range) {
    int ins_start = live_range[ins].first;
    int ins_end = live_range[ins].second;
    for (LLVMValueRef v_ins : sorted_list){
        int v_ins_start = live_range[v_ins].first;
        int v_ins_end = live_range[v_ins].second;
        // checking for overlap
        if (v_ins_start <= ins_end && v_ins_end >= ins_start){
            // checking if v_ins is in reg_map and the associated value is not -1
            if (reg_map.find(v_ins) != reg_map.end() && reg_map[v_ins] != -1){
                return v_ins;
            }
        }
    }
    return NULL;
}

// creates char* labels for each basic block based block_counter
std::unordered_map<LLVMBasicBlockRef, char*> createBBLabels(LLVMValueRef func){
    std::unordered_map<LLVMBasicBlockRef, char*> BBLabels;
    std::string curr_label = "";
    char* curr_label_char_ptr = NULL;
    int block_counter = 0; // the unique label for each is going to be generated based on the index they have
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)){
        curr_label = "BB" + std::to_string(block_counter);
        // converting to char*
        curr_label_char_ptr = strdup(curr_label.c_str());
        BBLabels[bb] = curr_label_char_ptr;
        block_counter++;
    }
    return BBLabels;
}

// printDirectives
void printDirectives(LLVMValueRef func, FILE* file_to_write){
    const char* func_label = LLVMGetValueName(func);
    fprintf(file_to_write, ".text\n.globl %s\n.type %s, @function\n%s:\n", func_label, func_label, func_label);
}

// printFunctionEnd

// populates offset_map which associated a value(instruction) to the memory offset it has from %ebp and gets localMem
std::pair<std::unordered_map<LLVMValueRef, int>, int> getOffsetMap(LLVMModuleRef module){
    std::unordered_map<LLVMValueRef, int> offset_map;
    // number of bytes required to store the local values being initialized to 4
    int localMem = 4;
    // since there is just one function the following loop will run once
    for (LLVMValueRef func = LLVMGetFirstFunction(module); func != NULL; func = LLVMGetNextFunction(func)){
        if (LLVMCountParams(func) > 0){
            // we only have one param by specifications of mini c so we use it
            LLVMValueRef parameter = LLVMGetParam(func, 0);
            offset_map[parameter] = 8;
        }
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)){
            for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
                // handling case depending on the type of instruction

                if (LLVMGetInstructionOpcode(ins) == LLVMAlloca) {
                    localMem += 4;
                    offset_map[ins] = -1*localMem;
                }
                if (LLVMGetInstructionOpcode(ins) == LLVMStore){
                    LLVMValueRef first_operand = LLVMGetOperand(ins, 0);
                    LLVMValueRef second_operand = LLVMGetOperand(ins, 1);
                    // case when the first operand of the store instruction is equal to the function parameter
                    if (LLVMCountParams(func) > 0 && first_operand == LLVMGetParam(func, 0)){
                        // getting the value associated to the first operand
                        int x = offset_map[first_operand];
                        // changing the value associated with the second operand to be x
                        offset_map[second_operand] = x;
                    }
                    // case when the first operand of the store instruction is not equal to the parameter and is not constant
                    else if (!LLVMIsConstant(first_operand)){
                        // getting the value associated with the second operand in offset_map
                        int x = offset_map[second_operand];
                        // adding the first operand as key with the associated value as x in offset_map
                        offset_map[first_operand] = x;
                    }
                }
                if (LLVMGetInstructionOpcode(ins) == LLVMLoad){
                    // getting the value associated with the first operand in the offset_map and naming it x
                    int x = offset_map[LLVMGetOperand(ins, 0)];
                    // adding ins as the key with the associated value as x in the offset_map
                    offset_map[ins] = x;
                }
            }
        }
    }
    return {offset_map, localMem};
}