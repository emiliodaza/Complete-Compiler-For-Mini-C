#include <stdio.h>
#include <stdbool.h>
#include <llvm-c/Core.h>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <llvm-c/IRReader.h>
#include <string.h>

bool run_common_subexpression_elimination(LLVMBasicBlockRef bb);
bool run_constant_folding(LLVMBasicBlockRef bb);
bool run_dead_code_elimination(LLVMValueRef func);
bool is_store_in_between_shared_memory_uses(LLVMValueRef first_instruction, LLVMValueRef second_instruction);
std::unordered_set <LLVMValueRef> compute_gen_set_for_block(LLVMBasicBlockRef bb);
std::unordered_set <LLVMValueRef> set_of_all_store (LLVMValueRef func);
std::unordered_set <LLVMValueRef> compute_kill_set_for_block(LLVMBasicBlockRef bb);
std::unordered_map <LLVMBasicBlockRef, std::unordered_set <LLVMBasicBlockRef>> compute_predecesor_blocks (LLVMValueRef func);
bool taking_load_into_consideration(LLVMValueRef func);
bool instruction_should_be_kept(LLVMValueRef instruction);
void constant_propagation_and_constant_folding(LLVMValueRef func);
std::unordered_map <LLVMBasicBlockRef, struct IN_and_OUT> in_and_out_sets_map(LLVMValueRef func);

struct IN_and_OUT {
    std::unordered_set <LLVMValueRef> in_set;
    std::unordered_set <LLVMValueRef> out_set;
};

// main has been erased since now main_function.cpp executes everything 

// functions for local tasks of optimization

bool run_common_subexpression_elimination(LLVMBasicBlockRef bb){
    bool replacement_has_happened = false;
    // If we find that a instruction is repeated we substitute the later
    //  reference with the first one so 
    // that dead code elimination later simplifies the code
    for (LLVMValueRef ins_1 = LLVMGetFirstInstruction(bb); ins_1 != NULL; ins_1 = LLVMGetNextInstruction(ins_1)) {

        for (LLVMValueRef ins_2 = LLVMGetNextInstruction(ins_1); ins_2 != NULL;) {

            LLVMValueRef ins_2_next = LLVMGetNextInstruction(ins_2);
            LLVMOpcode type_of_ins_1 = LLVMGetInstructionOpcode(ins_1);

            if (type_of_ins_1 == LLVMGetInstructionOpcode(ins_2) && (type_of_ins_1 == LLVMAdd || type_of_ins_1 == LLVMMul || type_of_ins_1 == LLVMSub)) {

                LLVMOpcode shared_type_of_ins = type_of_ins_1;
                bool operands_are_the_same_in_order = (LLVMGetOperand(ins_1, 0) == LLVMGetOperand(ins_2, 0) && LLVMGetOperand(ins_1, 1) == LLVMGetOperand(ins_2, 1));
                bool operands_are_the_same_in_opposite_order = (LLVMGetOperand(ins_1, 0) == LLVMGetOperand(ins_2, 1) && LLVMGetOperand(ins_1, 1) == LLVMGetOperand(ins_2, 0));
                bool operands_are_communitatively_the_same = (operands_are_the_same_in_order || operands_are_the_same_in_opposite_order);

                // b/c addition and multiplication are commutative so they are the same subexpression if the sets with operands in the instructions are equal
                if (operands_are_communitatively_the_same && (shared_type_of_ins == LLVMAdd || shared_type_of_ins == LLVMMul)) {
                    LLVMReplaceAllUsesWith(ins_2, ins_1);
                    replacement_has_happened = true; // we have replacement_has_happened in each if statement content b/c if we have the case where the operands the the same in opposite order but shared_type_of_ins is substraction then we cannot substitute 
                }
                // we handle substraction separately b/c substraction is not commutative so it requires the same order
                if (operands_are_the_same_in_order && shared_type_of_ins == LLVMSub){
                    LLVMReplaceAllUsesWith(ins_2, ins_1);
                    replacement_has_happened =true;
                }
            }

            // we separate these conditionals for readability

            if ((type_of_ins_1 == LLVMLoad && LLVMGetInstructionOpcode(ins_2) == LLVMLoad) && (LLVMGetOperand(ins_1, 0) == LLVMGetOperand(ins_2, 0))){
                if (!is_store_in_between_shared_memory_uses(ins_1, ins_2)){
                    LLVMReplaceAllUsesWith(ins_2, ins_1);
                    replacement_has_happened = true;
                }
            }

            ins_2 = ins_2_next;
        }
    }
    return replacement_has_happened;
}

bool is_store_in_between_shared_memory_uses(LLVMValueRef first_instruction, LLVMValueRef second_instruction) {
    
    // we get the shared pointer to the memory address
    LLVMValueRef memory_ptr = LLVMGetOperand(first_instruction, 0);

    for (LLVMValueRef instruction = LLVMGetNextInstruction(first_instruction); 
        instruction != NULL && instruction != second_instruction; 
        instruction = LLVMGetNextInstruction(instruction)) {
            LLVMOpcode opcode = LLVMGetInstructionOpcode(instruction);
            if (opcode == LLVMStore && memory_ptr == LLVMGetOperand(instruction, 1)) {
                return true; // meaning that there is an store operation with the same operand in between
            }
    }
    return false;
}

bool run_constant_folding(LLVMBasicBlockRef bb){
    bool changed = false; // to indicate if constant folding has been performed

    LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
    LLVMOpcode type_of_ins;
    while (instruction != NULL){
        type_of_ins = LLVMGetInstructionOpcode(instruction);
        LLVMValueRef next = LLVMGetNextInstruction(instruction); // to continue iterating later
        if (type_of_ins == LLVMAdd || type_of_ins == LLVMSub || type_of_ins == LLVMMul){
            
            LLVMValueRef first_operand = LLVMGetOperand(instruction, 0);
            LLVMBool is_first_operand_cons = LLVMIsConstant(first_operand);
            LLVMValueRef second_operand = LLVMGetOperand(instruction, 1);
            LLVMBool is_second_operand_cons = LLVMIsConstant(second_operand);

            LLVMValueRef folded_result = NULL;
            long long result; // which is then converted into LLVMValueRef to store folded_result
            
            if (is_first_operand_cons && is_second_operand_cons){
                long long first_operand_value = LLVMConstIntGetSExtValue(first_operand);
                long long second_operand_value = LLVMConstIntGetSExtValue(second_operand);
                if (type_of_ins == LLVMAdd){
                    result = first_operand_value + second_operand_value;
                }

                if (type_of_ins == LLVMSub){
                    result = first_operand_value - second_operand_value;
                }

                if (type_of_ins == LLVMMul){
                    result = first_operand_value * second_operand_value;
                }
                folded_result = LLVMConstInt(LLVMTypeOf(first_operand), result, 1);
            }
            if (folded_result != NULL){
                LLVMReplaceAllUsesWith(instruction, folded_result);
                LLVMInstructionEraseFromParent(instruction);
                changed = true;
            }
            
        }
        instruction = next;
    }
    return changed;
}

bool instruction_should_be_kept(LLVMValueRef instruction){
    LLVMOpcode ins_type = LLVMGetInstructionOpcode(instruction);
    // if we have any of the following cases then they are relevant in control flow and memory allocation
    // thus removing them would be dangerous for the successful execution of the program
    if (LLVMIsATerminatorInst(instruction) != NULL || ins_type == LLVMStore || ins_type == LLVMCall) {
        return true;
    }

    return false;
}


bool run_dead_code_elimination(LLVMValueRef func){
    // has_changed useful to get to the fixed point state. If we have reached a 
    // cycle on which there is no change then all dead code elimination has been performed
    bool has_changed_this_cycle = true;  // we initialize to true just to start the loop
    bool has_changed_at_all = false;

    while (has_changed_this_cycle){

        has_changed_this_cycle = false;

        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)) {

            for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst != NULL; ) {
                LLVMValueRef next = LLVMGetNextInstruction(inst);
                
                bool is_unused = (LLVMGetFirstUse(inst) == NULL);

                if (is_unused && !instruction_should_be_kept(inst)) {

                    LLVMInstructionEraseFromParent(inst);

                    has_changed_this_cycle = true;
                    has_changed_at_all = true;
                }

                inst = next;
            }
        }
    }
    return has_changed_at_all;
}

// computing the set GEN[B] for a basic block B
std::unordered_set <LLVMValueRef> compute_gen_set_for_block(LLVMBasicBlockRef bb) {
    std::unordered_set <LLVMValueRef> block_gen_set = {};
    // we go over each instruction in the basic block
    for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)) {
        LLVMOpcode type_of_ins = LLVMGetInstructionOpcode(ins);
        // if a instruction of type store is found then it is inserted
        if (type_of_ins == LLVMStore) {
            block_gen_set.insert(ins);
            LLVMValueRef ptr = LLVMGetOperand(ins, 1);
            LLVMValueRef ins_to_eliminate = NULL;
            // if any other instruction that references to the same ptr was already in place it is erased
            // one at a time
            for (LLVMValueRef element : block_gen_set) {
                if (ptr == LLVMGetOperand(element, 1) && element != ins) {
                    ins_to_eliminate = element;
                    break;
                }
            }

            if (ins_to_eliminate != NULL){
                block_gen_set.erase(ins_to_eliminate);
            }
        }
    }
    return block_gen_set;
} 

// computing the set of all store instructions in the function
std::unordered_set <LLVMValueRef> set_of_all_store (LLVMValueRef func) {
    std::unordered_set <LLVMValueRef> set_of_all_store = {};
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)){
        for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
            LLVMOpcode type_of_ins = LLVMGetInstructionOpcode(ins);
            if (type_of_ins == LLVMStore) {
                set_of_all_store.insert(ins);
            }
        }
    }
    return set_of_all_store;
}

// computing the set KILL[B] for a basic block B
// for each store instruction in the basic block to a pointer, the kill set is the set of all other store instructions to the same pointer in the entire program
std::unordered_set <LLVMValueRef> compute_kill_set_for_block(LLVMBasicBlockRef bb) {

    std::unordered_set <LLVMValueRef> block_kill_set = {};

    // getting the function to determine the set of all stores
    LLVMValueRef func = LLVMGetBasicBlockParent(bb);
    std::unordered_set <LLVMValueRef> set_of_stores = set_of_all_store(func);

    for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)){
        if (LLVMGetInstructionOpcode(ins) == LLVMStore){
            for (LLVMValueRef store_ins : set_of_stores) {
                if ((LLVMGetOperand(ins, 1) == LLVMGetOperand(store_ins, 1)) && (ins != store_ins) ) { // so that all the others are added
                    block_kill_set.insert(store_ins);
                }
            }
        }
    }
    return block_kill_set;
}

// Getting the predecessors map where the keys are the blocks and the values are their corresponding predecessors
std::unordered_map <LLVMBasicBlockRef, std::unordered_set <LLVMBasicBlockRef>> compute_predecesor_blocks (LLVMValueRef func) {
    std::unordered_map <LLVMBasicBlockRef, std::unordered_set <LLVMBasicBlockRef>> predecessors_map;

    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef terminator = LLVMGetBasicBlockTerminator(bb); // getting the terminator to access the successors
        if (terminator == NULL) {
            continue;
        }
        unsigned successor_index = 0;
        while (successor_index < LLVMGetNumSuccessors(terminator)) {
            LLVMBasicBlockRef successor = LLVMGetSuccessor(terminator, successor_index);
            predecessors_map[successor].insert(bb); // because the successor's predecessor is the current block
            successor_index++;
        }
    }
    return predecessors_map;
}

std::unordered_map <LLVMBasicBlockRef, struct IN_and_OUT> in_and_out_sets_map(LLVMValueRef func) {
    // we begin by computing the predecessors for each block for easier later computation
    std::unordered_map <LLVMBasicBlockRef, std::unordered_set <LLVMBasicBlockRef>> predecessors_map = compute_predecesor_blocks(func);
    // initializing each "in set" as empty and also initializing each "out set" as the gen set
    std::unordered_map <LLVMBasicBlockRef, struct IN_and_OUT> in_and_out_sets_map;

    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func);
        bb != NULL;
        bb = LLVMGetNextBasicBlock(bb)) {
            std::unordered_set <LLVMValueRef> in_set = {};
            std::unordered_set <LLVMValueRef> out_set = compute_gen_set_for_block(bb); 

            struct IN_and_OUT IN_and_OUT_element;
            // we provide the in_set and out_set data to the IN_and_OUT_element
            IN_and_OUT_element.in_set = in_set;
            IN_and_OUT_element.out_set = out_set;

            in_and_out_sets_map[bb] = IN_and_OUT_element;
        } 

    bool change = true; // in order to stop when we have reached a fixed point

    while (change) {
        change = false; // if a change is found this will change to true again
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL;  bb = LLVMGetNextBasicBlock(bb)) {

                // GEN and KILL set for block to do OUT[B] = GEN[B] union (in[B] - kill[B])
                std::unordered_set <LLVMValueRef> gen_set_of_block = compute_gen_set_for_block(bb);
                std::unordered_set <LLVMValueRef> kill_set_of_block = compute_kill_set_for_block(bb);

                std::unordered_set <LLVMBasicBlockRef> predecessors_of_bb = predecessors_map[bb];

                // Storing the latest OUT and IN of the block to compare with the new one and see if change has ocurred
                std::unordered_set <LLVMValueRef> latest_in_set_for_bb = in_and_out_sets_map[bb].in_set;
                std::unordered_set <LLVMValueRef> latest_out_set_for_bb = in_and_out_sets_map[bb].out_set;

                // We consider the new sets that will be constructed and keep the latest as above for comparison purposes (the new ones have to be cleared to be filled per each cycle)
                std::unordered_set <LLVMValueRef> new_in_set_for_bb = {};
                std::unordered_set <LLVMValueRef> new_out_set_for_bb = {};

                // computing and storing IN[B] as the union of the predecessors OUT
                for (LLVMBasicBlockRef predecessor : predecessors_of_bb) {
                    new_in_set_for_bb.insert(in_and_out_sets_map[predecessor].out_set.begin(), in_and_out_sets_map[predecessor].out_set.end());
                }

                // first we include all the gen set elements associated
                new_out_set_for_bb.insert(gen_set_of_block.begin(), gen_set_of_block.end());

                // inserting IN[B] - KILL[B] to finish defining OUT[B]
                for (LLVMValueRef IN_instruction : new_in_set_for_bb) {
                    if (kill_set_of_block.find(IN_instruction) == kill_set_of_block.end()) {// if IN_instruction is not found then it returns kill.end() so we add it (set difference) 
                            new_out_set_for_bb.insert(IN_instruction);
                    }
                }

                // checking if a change has occured

                if ((latest_in_set_for_bb != new_in_set_for_bb) || (latest_out_set_for_bb != new_out_set_for_bb)) {
                    change = true;
                    in_and_out_sets_map[bb].in_set = new_in_set_for_bb;
                    in_and_out_sets_map[bb].out_set = new_out_set_for_bb;
                }
            }
    }
    return in_and_out_sets_map;
}

bool taking_load_into_consideration(LLVMValueRef func){
    std::unordered_map <LLVMBasicBlockRef, struct IN_and_OUT> in_set_and_out_set_map = in_and_out_sets_map(func);
    bool change_has_ocurred = false; // if we perform constant propagation and effectively certain load instructions are liminated then we notify to the caller that a change has happened
    
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)) {
        
        std::unordered_set <LLVMValueRef> R = in_set_and_out_set_map[bb].in_set;
        // filled in the loop for instructions
        std::unordered_set <LLVMValueRef> marked_load_instructions_to_delete = {};
        
        for (LLVMValueRef ins = LLVMGetFirstInstruction(bb); ins != NULL; ins = LLVMGetNextInstruction(ins)) {

            if (LLVMGetInstructionOpcode(ins) == LLVMStore){

                R.insert(ins);

                // variable created just for the purpose of identifying which instruction to erase
                std::unordered_set <LLVMValueRef> to_erase = {};

                for (LLVMValueRef ins_in_R : R) {
                    
                    bool refers_to_the_same_ptr = (LLVMGetOperand(ins, 1) == LLVMGetOperand(ins_in_R, 1));
                    bool is_different_from_ins = (ins != ins_in_R);
                    
                    if (refers_to_the_same_ptr && is_different_from_ins) {
                        to_erase.insert(ins_in_R);
                    }
                }

                // we now delete the killed instructions from R
                for (LLVMValueRef ins_to_erase_from_R : to_erase) {
                    R.erase(ins_to_erase_from_R); 
                }
            }

            if (LLVMGetInstructionOpcode(ins) == LLVMLoad) {
                LLVMValueRef ptr = LLVMGetOperand(ins, 0);

                // finding all store instructions in R that write to the ptr
                std::unordered_set <LLVMValueRef> ins_in_R_to_ptr = {};

                for (LLVMValueRef ins_in_R : R) {
                    if (LLVMGetOperand(ins_in_R, 1) == ptr) {
                        ins_in_R_to_ptr.insert(ins_in_R);
                    }
                }

                // checking if all of the instructions in R to ptr are the same constant and if they are constant store instructions

                bool are_all_the_same_constant = true; // becomes false if we find a counterexample
                bool is_current_constant_initialized = false;
                long long current_constant;

                if (!ins_in_R_to_ptr.empty()) {

                    for (LLVMValueRef ins_to_check : ins_in_R_to_ptr) {
                        LLVMValueRef associated_value = LLVMGetOperand(ins_to_check, 0);
                        if (!LLVMIsAConstantInt(associated_value)){
                            are_all_the_same_constant = false; // it is not a constant store instruction so it becomes false
                            break;
                        }
                        if (!is_current_constant_initialized){
                            current_constant = LLVMConstIntGetSExtValue(associated_value); // to start checking
                            is_current_constant_initialized = true;
                        } else {
                            // they are compared as long long to avoid pointer comparison
                            if (current_constant != LLVMConstIntGetSExtValue(associated_value)) {
                                are_all_the_same_constant = false; // we found a counter example where the constant is not the same
                                break;
                            }
                        }
                    }
                }

                if(are_all_the_same_constant && is_current_constant_initialized) {
                    // converting long long current_constant back to LLVMValueRef to apply LLVMReplaceAllUsesWith
                    LLVMTypeRef Ty = LLVMTypeOf(ins); // to get the type of integer the load instruction refers to like i32
                    LLVMValueRef current_constant_value = LLVMConstInt(Ty, (unsigned long long) current_constant, 1);
                    LLVMReplaceAllUsesWith(ins, current_constant_value);
                    change_has_ocurred = true;
                    marked_load_instructions_to_delete.insert(ins); // since it was already substituted by current_constant_value
                }
            }
        }
        
        for (LLVMValueRef load_to_delete : marked_load_instructions_to_delete) {
            LLVMInstructionEraseFromParent(load_to_delete);
        }
    }
    return change_has_ocurred;
}

void constant_propagation_and_constant_folding(LLVMValueRef func) {
    bool there_is_a_change = true; // becomes true when we encounter one, this boolean is useful to detect if we have reached a fixed point
    while (there_is_a_change) {
        // constant propagation and then constant folding
        bool change_of_type_1_occurred = taking_load_into_consideration(func);
        bool change_of_type_2_occurred = false; // gets updated based on the following loop
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)) {
            if (run_constant_folding(bb)) { // run_constant_folding returns true if there has been a change and false otherwise
                change_of_type_2_occurred = true;
            }
        }
        if (!change_of_type_1_occurred && !change_of_type_2_occurred) { // no change happened then we are done
            there_is_a_change = false;
        }
    }
    run_dead_code_elimination(func); // in case we have dead code afterwards
}