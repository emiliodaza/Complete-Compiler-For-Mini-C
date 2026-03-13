/* Read the name of the file containing miniC program
Do the error checks to make sure the file is valid
Run yyparse to run syntax analysis and build the AST 
If the parser worked correctly and AST root is not NULL, run semantic analysis
If semantic analysis did not find any errors, run the IR builder to get the LLVM IR module
If the LLVM IR module is valid, run the optimizer
(Last assignment) Run the register allocation algorithm and assembly code generator 
Cleanup by calling yylex_destroy, freeNode on AST, LLVMDisposeModule, and LLVMShutdown  */

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "ast.h"

extern FILE* yyin;
extern int yyparse();
extern astNode* root;
extern int semantic_analysis(astNode* root);
extern void var_names_unique(astNode* prog_node);
extern LLVMModuleRef main_algorithm(astNode* prog_node);
extern bool run_common_subexpression_elimination(LLVMBasicBlockRef bb);
extern bool run_constant_folding(LLVMBasicBlockRef bb);
extern bool run_dead_code_elimination(LLVMValueRef func);
extern void constant_propagation_and_constant_folding(LLVMValueRef func);
extern std::unordered_map <LLVMValueRef, int> register_allocation_algorithm(LLVMModuleRef module);
extern void assembly_code_generation(LLVMModuleRef module, std::unordered_map<LLVMValueRef, int>& reg_map, FILE* file_to_write);
extern void yylex_destroy();
extern void freeNode(astNode* node);


int main(int argc, char* argv[]){
    if (argc != 2){
        fprintf(stderr, "You should only provide the name of one miniC program you intend to compile\n");
        exit(1);
    }
    char* filename = argv[1];
    // checking for proper extension: .c
    char* ptr_to_last_dot = strrchr(filename, '.');
    if (ptr_to_last_dot == NULL || strcmp(ptr_to_last_dot, ".c") != 0){
        fprintf(stderr, "You have not provided a file with the appropiate extension, it should be .c\n");
        exit(2);
    }
    // only checking for error when opening
    FILE* file_to_read = fopen(filename, "r");
    if (file_to_read == NULL){
        fprintf(stderr, "The file could not be opened.\n");
        exit(3);
    }

    yyin = file_to_read;
    int parse_result = yyparse();
    // checking syntax and if root has been modified as expected
    if (parse_result != 0 || root == NULL){
        fprintf(stderr, "Parsing encountered an error\n");
        fclose(file_to_read);
        exit(4);
    }
    // checking semantics
    if (semantic_analysis(root) != 0){
        fprintf(stderr, "There has been a semantic error\n");
        fclose(file_to_read);
        exit(5);
    }
    // IR Builder stage
    var_names_unique(root);
    LLVMModuleRef module = main_algorithm(root);
    // checking if valid module
    char* err = NULL;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &err)){
        fprintf(stderr, "The module is not valid: %s\n", err);
        LLVMDisposeMessage(err);
        exit(6);
    }
    LLVMDisposeMessage(err);
    // optimization step
    for (LLVMValueRef func = LLVMGetFirstFunction(module); func != NULL; func = LLVMGetNextFunction(func)) {
        if (LLVMCountBasicBlocks(func) == 0) { // there is nothing to process so we continue
            continue;
        }
        // local optimizations
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func); bb != NULL; bb = LLVMGetNextBasicBlock(bb)) {
            run_common_subexpression_elimination(bb);
            run_constant_folding(bb);
        }
        run_dead_code_elimination(func);
        // global optimization until fixed point
        constant_propagation_and_constant_folding(func);
    }
    // register allocation & assembly
    std::unordered_map <LLVMValueRef, int> reg_map = register_allocation_algorithm(module);
    // creating output_filename .s
    std::string output_filename = std::string(filename, ptr_to_last_dot) + ".s";
    FILE* file_to_write = fopen(strdup(output_filename.c_str()), "w");
    if (file_to_write == NULL){
        fprintf(stderr, "Could not open output file\n");
        exit(7);
    }
    assembly_code_generation(module, reg_map, file_to_write);
    fclose(file_to_read);
    fclose(file_to_write);
    yylex_destroy();
    freeNode(root);
    LLVMDisposeModule(module);
    LLVMShutdown();
    return 0;
}