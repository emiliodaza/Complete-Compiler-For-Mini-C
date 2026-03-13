#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include "ast/ast.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector> // to keep track of old and new names in scopes
#include <queue> // for BFS later

void var_names_unique(astNode* prog_node);
void helper_rename_stmt_list(vector<astNode*>* stmt_list);
void helper_rename_stmt(astNode* stmt_astNode);
void helper_rename_expr(astNode* expression);
LLVMModuleRef main_algorithm(astNode* prog_node);
LLVMBasicBlockRef genIRStmt(astNode* statement_node, LLVMBuilderRef builder, LLVMBasicBlockRef startBB);
LLVMValueRef genIRExpr(astNode* expression, LLVMBuilderRef builder);
void set_insert_local_vars(std::unordered_set<std::string>* set_to_modify, vector<astNode*> * stmt_list);

// relevant objects that are meant to be accessible for functions beyond the local scope
LLVMTypeRef int_data_type;
LLVMTypeRef void_data_type;
std::unordered_map <std::string, LLVMValueRef> var_map;
LLVMValueRef main_function;
LLVMTypeRef read_type;
LLVMValueRef readFunc;
LLVMTypeRef print_type;
LLVMValueRef print_func;
LLVMValueRef ret_ref;
LLVMBasicBlockRef retBB;
LLVMContextRef context;

// using the counter approach to get unique names
int counter_as_suffix = 0;
// using vector of maps to have a map per each scope to analyze from the innermost
// scope outwards. Each map will have the old name as key and the new name
// as value
std::vector<std::unordered_map<std::string, std::string>> maps_of_scopes;

void var_names_unique(astNode* prog_node) {
    maps_of_scopes.push_back({});
    if (prog_node->type == ast_prog) {
        if ( prog_node->prog.func->func.param != NULL ) {
            char* old_name = prog_node->prog.func->func.param->var.name;
            std::string new_name_in_std_string = std::string(old_name) + "_" + std::to_string(counter_as_suffix);
            counter_as_suffix++;
             // updating the name and making it be char* through strdup() and .c_str()
            prog_node->prog.func->func.param->var.name = strdup(new_name_in_std_string.c_str());
            // we store the relation of old name and new name for the current scope (we convert to std::string to match map datatypes)
            maps_of_scopes.back()[std::string(old_name)] = new_name_in_std_string;
        };
    }

    // we call the helper to rename variables inside the stmt_list
    helper_rename_stmt_list(prog_node -> prog.func -> func.body -> stmt.block.stmt_list);
}

void helper_rename_stmt_list(vector<astNode*>* stmt_list) {
    // for each stmt in the stmt_list in the function body we inspect to rename
    for (astNode* stmt_astNode : *(stmt_list)) {
        helper_rename_stmt(stmt_astNode);
    }
}

void helper_rename_stmt(astNode* stmt_astNode) {
    if (stmt_astNode->stmt.type == ast_decl) {
        char* old_name = stmt_astNode->stmt.decl.name;
        std::string new_name_in_std_string = std::string(old_name) + "_" + std::to_string(counter_as_suffix);
        counter_as_suffix++;
        stmt_astNode->stmt.decl.name = strdup(new_name_in_std_string.c_str());
        maps_of_scopes.back()[std::string(old_name)] = new_name_in_std_string;
    } else if (stmt_astNode->stmt.type == ast_block) {
        // we introduce a new scope for analysis
        maps_of_scopes.push_back({});
        // make recursive call to check the stmt_list inside the block and do proper renaming
        helper_rename_stmt_list(stmt_astNode->stmt.block.stmt_list);
        maps_of_scopes.pop_back();
    } else if (stmt_astNode -> stmt.type == ast_while) {
        helper_rename_expr(stmt_astNode->stmt.whilen.cond);
        helper_rename_stmt(stmt_astNode->stmt.whilen.body);
    } else if (stmt_astNode -> stmt.type == ast_asgn) {
        helper_rename_expr(stmt_astNode->stmt.asgn.lhs);
        helper_rename_expr(stmt_astNode->stmt.asgn.rhs);
    } else if (stmt_astNode -> stmt.type == ast_if) {
        helper_rename_expr(stmt_astNode->stmt.ifn.cond);
        helper_rename_stmt(stmt_astNode->stmt.ifn.if_body);
        if (stmt_astNode->stmt.ifn.else_body != NULL) {
            helper_rename_stmt(stmt_astNode->stmt.ifn.else_body);
        }
    } else if (stmt_astNode -> stmt.type == ast_ret) {
        helper_rename_expr(stmt_astNode->stmt.ret.expr);
    } else if (stmt_astNode -> stmt.type == ast_call) {
        helper_rename_expr(stmt_astNode->stmt.call.param);
    }
}

void helper_rename_expr(astNode* expression) {
    if (expression == NULL) return;
    if (expression->type == ast_var) {
        char* old_name = expression->var.name;
        char* new_name = old_name; // we initialize to old_name which gets updated as we encounter the new name of its declared variable
        // checking from the innermost scope to the outermost to update var name
        for (int i = maps_of_scopes.size()-1; i >= 0; i--) {
            // using std::string datatypes in the map to compare actual content rather than 
            // the pointers
            if (maps_of_scopes[i].count(std::string(old_name)) != 0) { // check if the key exists in the map
                std::string new_name_in_std_string = maps_of_scopes[i][std::string(old_name)];
                new_name = strdup(new_name_in_std_string.c_str());
                break;
            }
        }
        expression->var.name = new_name;
    } else if (expression->type == ast_rexpr) {
        helper_rename_expr(expression->rexpr.lhs);
        helper_rename_expr(expression->rexpr.rhs);
    } else if (expression->type == ast_bexpr) {
        helper_rename_expr(expression->bexpr.lhs);
        helper_rename_expr(expression->bexpr.rhs);
    } else if (expression->type == ast_uexpr) {
        helper_rename_expr(expression->uexpr.expr);
    }
    // the other types are already handled before reaching this point like prog, func, or statement.
}

// main algorithm
LLVMModuleRef main_algorithm(astNode* prog_node) {
    // creating context to store all LLVM IR objects
    context = LLVMContextCreate();
    printf("1. Context created...\n"); // debug print statements

    // getting the architecture from the machine
    char* triple = LLVMGetDefaultTargetTriple();
    
    // generating module and setting target architecture
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("module",
                                                context);
    printf("2. Module created...\n");

    LLVMSetTarget(module, triple);

    LLVMDisposeMessage(triple);
    
    // generating LLVMFunctions without bodies for print and read
    // creating data types definitions
    int_data_type = LLVMInt32TypeInContext(context);
    void_data_type = LLVMVoidTypeInContext(context);

    // int read 
    read_type = LLVMFunctionType(int_data_type, NULL, 0, 0);
    readFunc = LLVMAddFunction(module, "read", read_type);
    
    // void print(int)
    LLVMTypeRef params_for_print[] = {int_data_type};
    print_type = LLVMFunctionType(void_data_type, params_for_print, 1, 0);
    print_func = LLVMAddFunction(module, "print", print_type);

    // visiting the function node of the astProg node
    astNode* function_node = prog_node -> prog.func;
    printf("Function node accessed...\n");
    
    // creating LLVM builder
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);

    // reconstructing the function for LLVM IR

    // extracting type of params and count (if at least one then it is int otherwise it is void)

    LLVMTypeRef* param_types;
    unsigned param_count;
    astNode* pointer_to_param = function_node -> func.param;

    if (pointer_to_param == NULL) {
        param_types = NULL;
        param_count = 0; // since the pointer is NULL then it doesn't take any params
    } else {
        param_types = &int_data_type;
        param_count = 1;
    }

    LLVMTypeRef function_type = LLVMFunctionType(int_data_type,
                             param_types, param_count,
                             0);
    
    main_function = LLVMAddFunction(module, "main_function", function_type);

    // generating the entry basic block
    LLVMBasicBlockRef entryBB = LLVMAppendBasicBlockInContext(context,
                                                main_function,
                                                "main_function_body");
    printf("EntryBB created...\n");
    // creating set with names of all parameters and local variables
    std::unordered_set <std::string> names_of_params_and_local_vars;
    
    // adding parameter if applicable
    if (param_count != 0) {
        char* name_of_param = pointer_to_param -> var.name;
        names_of_params_and_local_vars.insert(name_of_param);
        printf("Param added...\n");
    }
    // adding local variables if applicable
    astNode* body = function_node -> func.body;
    if (body->type == ast_stmt && body->stmt.type == ast_block) {
        vector<astNode*> * stmt_list = body -> stmt.block.stmt_list;
        // call to helper method to continue filling the set
        set_insert_local_vars(&names_of_params_and_local_vars, stmt_list);
        printf("Local vars added...\n");
    }

    // setting the position of the builder at the end of entryBB
    LLVMPositionBuilderAtEnd(builder, entryBB);

    // initializing var_map to an empty map
    var_map = {};
    for (std::string name_of_var : names_of_params_and_local_vars){
        LLVMValueRef alloca_object = LLVMBuildAlloca(builder, int_data_type, name_of_var.c_str());
        var_map[name_of_var] = alloca_object;
    }
    printf("Allocas created...\n");
    // generating the alloc instruction for the return value
    ret_ref = LLVMBuildAlloca(builder, int_data_type, "ret_ref");
    // generating the store instruction to store function parameter into respective alloca ptr
    if (param_count != 0) {
        // we get the param in LLVMValueRef type to feed as input to LLVMBuildStore
        LLVMValueRef main_function_llvm_param = LLVMGetParam(main_function, 0);
        
        // getting the relevant_alloca LLVMValueRef object that corresponds to the parameter
        char* name_of_param_for_lookup = pointer_to_param -> var.name;
        LLVMValueRef relevant_alloca = var_map[name_of_param_for_lookup];

        LLVMValueRef store_instruction_for_param = LLVMBuildStore(builder, main_function_llvm_param, relevant_alloca);
    }
    
    // generating retBB as the return basic block
    retBB = LLVMAppendBasicBlockInContext(context, main_function, "return");
    printf("retBB created...\n");

    // setting the position of builder at the end of retBB
    LLVMPositionBuilderAtEnd(builder, retBB);

    // adding a load instruction from the ret_ref to retBB
    LLVMValueRef load_ins_to_retBB = LLVMBuildLoad2(builder, int_data_type, ret_ref,  "load");

    // adding return instruction for the load instruction
    LLVMValueRef return_ins = LLVMBuildRet(builder, load_ins_to_retBB);

    printf("Starting genIRStmt...\n");
    // generating IR for the function body
    LLVMBasicBlockRef exitBB = genIRStmt(body, builder, entryBB);
    printf("Completed genIRStmt...\n");

    // getting termiantor instruction of exitBB
    LLVMValueRef exitBB_terminator = LLVMGetBasicBlockTerminator(exitBB);
    if (exitBB_terminator == NULL) {
        LLVMPositionBuilderAtEnd(builder, exitBB);
        LLVMBuildBr(builder, retBB);
    }

    // removing all basic blocks that do not have any predecessor basic blocks
    // performing BFS to determine all the blocks that are in the connected component (contained in visited)
    std::queue<LLVMBasicBlockRef> q;
    q.push(entryBB); // we start with entry block
    std::unordered_set<LLVMBasicBlockRef> visited; // any basic block which is not here would mean it does not have predecessor blocks
    while (!q.empty()) {
        LLVMBasicBlockRef curr_bb = q.front();
        q.pop();
        visited.insert(curr_bb);

        LLVMValueRef curr_terminator = LLVMGetBasicBlockTerminator(curr_bb);
        int index_of_successor = 0;
        int total_successors_for_curr = LLVMGetNumSuccessors(curr_terminator);
        while (index_of_successor < total_successors_for_curr) {
            // if we get visited.end() that means the successor is not found in visited so we add it to the queue 
            // and mark as visited
            if (visited.find(LLVMGetSuccessor(curr_terminator, index_of_successor)) == visited.end()){
                q.push(LLVMGetSuccessor(curr_terminator, index_of_successor));
                visited.insert(LLVMGetSuccessor(curr_terminator, index_of_successor));
            }
            index_of_successor++;
        }
    }
    printf("BFS completed...\n");
    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(main_function);
    while (bb != NULL) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        // if the bb is not found in visited at this point that means it does not have predecessors
        // so we delete it and continue with the next one to check
        if (visited.find(bb) == visited.end()){
            LLVMDeleteBasicBlock(bb);
        }
        bb = next_bb;
    }
    printf("Deletion of disconnected basic blocks finished...\n");
    LLVMDisposeBuilder(builder);
    return module;
}

void set_insert_local_vars(std::unordered_set<std::string>* set_to_modify, vector<astNode*> * stmt_list) {

    if (stmt_list == NULL) {
        return;
    }

    for (astNode* stmt_node : *stmt_list) {
        astStmt stmt = stmt_node -> stmt;
        // base case: if the node is a declaration we just insert it
        if (stmt.type == ast_decl) {
            char* stmt_name = stmt.decl.name;
            set_to_modify->insert(stmt_name);
        }

        // everytime we encounter a type of statement that can contain a stmt_list we recurse to process its potential declarations

        if (stmt.type == ast_block) {
            vector<astNode*> * stmt_list_inner_block = stmt.block.stmt_list;
            set_insert_local_vars(set_to_modify, stmt_list_inner_block);
        }

        if (stmt.type == ast_while) {
            astNode* while_body = stmt.whilen.body;
            if (while_body != NULL) {
                if (while_body->type == ast_stmt){
                    if (while_body->stmt.type == ast_block) {
                        vector<astNode*>* stmt_list_while_block = while_body -> stmt.block.stmt_list;
                        set_insert_local_vars(set_to_modify, stmt_list_while_block);
                    }
                }
            }
        }

        if (stmt.type == ast_if) {

            // recurse the if body
            astNode* if_body = stmt.ifn.if_body;
            if (if_body != NULL) {
                if (if_body->type == ast_stmt) {
                    if (if_body->stmt.type == ast_block){
                        vector<astNode*>* stmt_list_if_body = if_body -> stmt.block.stmt_list;
                        set_insert_local_vars(set_to_modify, stmt_list_if_body);
                    }
                }
            }

            // recurse the else body if existent
            astNode* else_body = stmt.ifn.else_body;
            if (else_body != NULL) {
                if (else_body->type == ast_stmt) {
                    if (else_body->stmt.type == ast_block){
                        vector<astNode*>* stmt_list_else_body = else_body -> stmt.block.stmt_list;
                        set_insert_local_vars(set_to_modify, stmt_list_else_body);
                    }
                }
            }
        }
    }
}

LLVMBasicBlockRef genIRStmt(astNode* statement_node, LLVMBuilderRef builder, LLVMBasicBlockRef startBB) {
    // astStmt object
    astStmt stmt_object = statement_node -> stmt;
    if (stmt_object.type == ast_asgn) {
        LLVMPositionBuilderAtEnd(builder, startBB);
        // extracting the expression in the right hand side
        astNode* rhs = stmt_object.asgn.rhs;
        LLVMValueRef rhs_processed  = genIRExpr(rhs, builder);
        char* var_name = stmt_object.asgn.lhs->var.name;
        LLVMValueRef alloca_for_var = var_map[var_name];
        LLVMBuildStore(builder, rhs_processed, alloca_for_var);
        return startBB;
    } else if (stmt_object.type == ast_call) {
        LLVMPositionBuilderAtEnd(builder, startBB);
        // the value being printed is the parameter of associated call so we pass that as expression input to get its LLVMValueRef
        LLVMValueRef value_to_print = genIRExpr(stmt_object.call.param, builder);
        LLVMBuildCall2(builder, print_type, print_func, &value_to_print, 1, "call_to_print");
        return startBB;
    } else if (stmt_object.type == ast_while) {
        LLVMPositionBuilderAtEnd(builder, startBB);
        // generating a basic block to check the condition of the while loop
        LLVMBasicBlockRef condBB = LLVMAppendBasicBlockInContext(context, main_function, "condBB");
        // unconditional branch at the end of startBB to condBB
        LLVMBuildBr(builder, condBB);
        LLVMPositionBuilderAtEnd(builder, condBB);
        // generating LLVMValueRef of the relational expression in the condition of the while loop
        LLVMValueRef rexpr_while = genIRExpr(stmt_object.whilen.cond, builder);
        // generating two basic blocks that will be the successors when condition is true or false respectively
        LLVMBasicBlockRef trueBB = LLVMAppendBasicBlockInContext(context, main_function, "trueBB");
        LLVMBasicBlockRef falseBB = LLVMAppendBasicBlockInContext(context, main_function, "falseBB");
        // creating conditional branch to operate conditionally accordingly
        LLVMBuildCondBr(builder, rexpr_while, trueBB, falseBB);
        // generating LLVM IR for the body of the while loop 
        LLVMBasicBlockRef trueExitBB = genIRStmt(stmt_object.whilen.body, builder, trueBB);
        // setting position of builder to the end of trueExitBB
        LLVMPositionBuilderAtEnd(builder, trueExitBB);
        // generating unconditional branch to condBB at the end of trueExitBB
        LLVMBuildBr(builder, condBB);
        return falseBB;
    } else if (stmt_object.type == ast_if) {
        LLVMPositionBuilderAtEnd(builder, startBB);
        // generating LLVMValueRef of if condition
        LLVMValueRef rexpr_if = genIRExpr(stmt_object.ifn.cond, builder);
        // generating two basic blocks that will be successors depending on the condition truth value
        LLVMBasicBlockRef trueBB = LLVMAppendBasicBlockInContext(context, main_function, "trueBB");
        LLVMBasicBlockRef falseBB = LLVMAppendBasicBlockInContext(context, main_function, "falseBB");
        // generating conditional branch and setting successors accordingly
        LLVMBuildCondBr(builder, rexpr_if, trueBB, falseBB);
        // case for when else_body is not present
        if (stmt_object.ifn.else_body == NULL) {
            LLVMBasicBlockRef ifExitBB = genIRStmt(stmt_object.ifn.if_body, builder, trueBB);
            LLVMPositionBuilderAtEnd(builder, ifExitBB);
            LLVMBuildBr(builder, falseBB);
            return falseBB;
        } else { // case for when we do have an else_body
            LLVMBasicBlockRef ifExitBB = genIRStmt(stmt_object.ifn.if_body, builder, trueBB);
            LLVMBasicBlockRef elseExitBB = genIRStmt(stmt_object.ifn.else_body, builder, falseBB);
            LLVMBasicBlockRef endBB = LLVMAppendBasicBlockInContext(context, main_function, "endBB");
            LLVMPositionBuilderAtEnd(builder, ifExitBB);
            LLVMBuildBr(builder, endBB);
            LLVMPositionBuilderAtEnd(builder, elseExitBB);
            LLVMBuildBr(builder, endBB);
            return endBB;
        }
    } else if (stmt_object.type == ast_ret) {
        LLVMPositionBuilderAtEnd(builder, startBB);
        // generating LLVMValueRef of the return expr
        LLVMValueRef ret_expr = genIRExpr(stmt_object.ret.expr, builder);
        // generating store instruction from LLVMValueRef of return value to ret_ref
        LLVMBuildStore(builder, ret_expr, ret_ref);
        LLVMBuildBr(builder, retBB);
        LLVMBasicBlockRef endBB = LLVMAppendBasicBlockInContext(context, main_function, "endBB");
        return endBB;
    } else if (stmt_object.type == ast_block) {
        LLVMBasicBlockRef prevBB = startBB;
        for (astNode* stmt : *(stmt_object.block.stmt_list)){
            prevBB = genIRStmt(stmt, builder, prevBB);
        }
        return prevBB;
    } else if (stmt_object.type == ast_decl){
        return startBB; // it is not modified b/c all the vars and params alloca instructinos were already created
    }
    return NULL;
}

LLVMValueRef genIRExpr(astNode* expression, LLVMBuilderRef builder) {
    if (expression -> type == ast_cnst) {
        int const_value = expression -> cnst.value;
        LLVMValueRef llvm_constant = LLVMConstInt(int_data_type, const_value, 0);

        return llvm_constant;

    } else if (expression -> type == ast_var) {
        char* name_of_var = expression -> var.name;
        LLVMValueRef memory_location = var_map[name_of_var];
        LLVMValueRef load_instruction = LLVMBuildLoad2(builder, int_data_type, memory_location, name_of_var);

        return load_instruction;

    } else if (expression -> type == ast_uexpr) {
        astNode* uexpression = expression -> uexpr.expr;
        // recursion for the expr in the unary expression node and storing the simplified value
        LLVMValueRef simplified_expr_component = genIRExpr(uexpression, builder);
        // creating the constant 0 to make the unary expression
        LLVMValueRef zero_llvm = LLVMConstInt(int_data_type, 0, 0);
        // to finally get the unary
        LLVMValueRef unary = LLVMBuildSub(builder, zero_llvm, simplified_expr_component, "unary");

        return unary;

    } else if (expression -> type == ast_bexpr) {
        astNode* lhs = expression -> bexpr.lhs;
        LLVMValueRef processed_lhs = genIRExpr(lhs, builder);
        astNode* rhs = expression -> bexpr.rhs;
        LLVMValueRef processed_rhs = genIRExpr(rhs, builder);
        if ( expression -> bexpr.op == add ) {
            LLVMValueRef bexpr_addition = LLVMBuildAdd(builder, processed_lhs, processed_rhs, "bexpr_addition");

            return bexpr_addition;

        } else if (expression -> bexpr.op == sub) {
            LLVMValueRef bexpr_sub = LLVMBuildSub(builder, processed_lhs, processed_rhs, "bexpr_substraction");

            return bexpr_sub;

        } else if (expression -> bexpr.op == divide) {
            LLVMValueRef bexpr_div = LLVMBuildSDiv(builder, processed_lhs, processed_rhs, "bexpr_div");

            return bexpr_div;

        } else if (expression -> bexpr.op == mul) {
            LLVMValueRef bexpr_mul = LLVMBuildMul(builder, processed_lhs, processed_rhs, "bexpr_mul");

            return bexpr_mul;

        }
    } else if (expression -> type == ast_rexpr) {
        astNode* lhs = expression -> rexpr.lhs;
        LLVMValueRef lhs_processed = genIRExpr(lhs, builder);
        astNode* rhs = expression -> rexpr.rhs;
        LLVMValueRef rhs_processed = genIRExpr(rhs, builder);

        if (expression->rexpr.op == lt) {
            LLVMValueRef lt_cmp = LLVMBuildICmp(builder, LLVMIntSLT, lhs_processed, rhs_processed, "lt_cmp");

            return lt_cmp;

        } else if (expression -> rexpr.op == gt) {
            LLVMValueRef gt_cmp = LLVMBuildICmp(builder, LLVMIntSGT, lhs_processed, rhs_processed, "gt_cmp");

            return gt_cmp;

        } else if (expression -> rexpr.op == le) {
            LLVMValueRef le_cmp = LLVMBuildICmp(builder, LLVMIntSLE, lhs_processed, rhs_processed, "le_cmp");

            return le_cmp;

        } else if (expression -> rexpr.op == ge) {
            LLVMValueRef ge_cmp = LLVMBuildICmp(builder, LLVMIntSGE, lhs_processed, rhs_processed, "ge_cmp");

            return ge_cmp;

        } else if (expression -> rexpr.op == eq) {
            LLVMValueRef eq_cmp = LLVMBuildICmp(builder, LLVMIntEQ, lhs_processed, rhs_processed, "eq_cmp");

            return eq_cmp;
            
        } else if (expression -> rexpr.op == neq) {
            LLVMValueRef neq_cmp = LLVMBuildICmp(builder, LLVMIntNE, lhs_processed, rhs_processed, "neq_cmp");

            return neq_cmp;

        }
    } else if (expression -> type == ast_stmt && expression -> stmt.type == ast_call) {
        // for the read function the param field will be NULL so the count of params is 0
        LLVMValueRef call_llvm = LLVMBuildCall2(builder, read_type, readFunc, NULL, 0, "call_llvm");

        return call_llvm;

    }
    return NULL;
}