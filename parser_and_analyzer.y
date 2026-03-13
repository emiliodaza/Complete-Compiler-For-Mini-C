%{
    #include <stdio.h>
    #include <stdlib.h>
    #include "ast.h"
    #include <vector> 
    #include <unordered_set>
    #include <string>
    #include <assert.h>

    astNode* root; // root note of AST
    std::vector<std::unordered_set<std::string>> scopes; // contains the symbol tables for scope

    int traverse(astNode* r);
    int yylex(void);
    void yyerror(const char *s);
    int semantic_analysis(astNode* root);
    int traverse(astNode* node);
    void get_into_scope();
    void get_out_of_scope();
    int declare(const char* name);
    int declared_in_any_scope(const char* name);
    int has_declaration_in_scope(const char* name);
    int semantic_error_notifier(const char* message, const char* name_of_associated_var);
    int traverse_stmt_list(std::vector<astNode*>* stmt_list);
%}

%define parse.error verbose
%code requires { // to guarantee they are considered in parser_analyzer.tab.h as they are part of %union
    #include "ast.h"
    #include <vector> 
}

%union {int ival; 
        char* sval; 
        std::vector<astNode*>* vec;
        astNode* node;}

%token <ival> NUM
%token <sval> IDENTIFIER
%token ADD MINUS DIVIDE MUL LE GE EQ
%token NEQ LT GT ASSIGN WHILE LEFT_PARENTHESIS
%token RIGHT_PARENTHESIS LEFT_CURLY RIGHT_CURLY
%token IF ELSE RETURN END_OF_STATEMENT PRINT READ
%token EXTERN INT_DECLARATION VOID_DECLARATION

%left LE GE EQ NEQ LT GT
%left ADD MINUS
%left DIVIDE MUL
%right UMINUS

// the following is done for two reasons: to avoid chained else's and
// to prevent shift-reduce errors in stmt

%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE 

%type <node> param_func_possibilities
%type <vec> stmt_list
%type <node> expression stmt block call program extern_print extern_read func 
%start program

%%
program     : extern_print extern_read func                               {$$ = createProg($1, $2, $3); root = $$;} 
            ;

expression  : expression DIVIDE expression                      {$$ = createBExpr($1, $3, divide);}
            | expression MUL expression                         {$$ = createBExpr($1, $3, mul);}
            | expression ADD expression                         {$$ = createBExpr($1, $3, add);}
            | expression MINUS expression                       {$$ = createBExpr($1, $3, sub);}
            | expression LE expression                          {$$ = createRExpr($1, $3, le);}
            | expression GE expression                          {$$ = createRExpr($1, $3, ge);}
            | expression EQ expression                          {$$ = createRExpr($1, $3, eq);}
            | expression NEQ expression                         {$$ = createRExpr($1, $3, neq);}
            | expression LT expression                          {$$ = createRExpr($1, $3, lt);}
            | expression GT expression                          {$$ = createRExpr($1, $3, gt);}
            | MINUS expression  %prec UMINUS                    {$$ = createUExpr($2, uminus);} // so that it is right associative and takes precedence
            | NUM                                               {$$ = createCnst($1);}
            | IDENTIFIER                                        {$$ = createVar($1);}
            | LEFT_PARENTHESIS expression RIGHT_PARENTHESIS     {$$ = $2;}
            | call                                              {$$ = $1;}
            ;

stmt        : WHILE LEFT_PARENTHESIS expression RIGHT_PARENTHESIS stmt                              {$$ = createWhile($3, $5);}
            | IF LEFT_PARENTHESIS expression RIGHT_PARENTHESIS stmt ELSE stmt                       {$$ = createIf($3, $5, $7);}
            | IF LEFT_PARENTHESIS expression RIGHT_PARENTHESIS stmt  %prec LOWER_THAN_ELSE          {$$ = createIf($3, $5);}
            | INT_DECLARATION IDENTIFIER END_OF_STATEMENT                                           {$$ = createDecl($2);}
            | RETURN expression END_OF_STATEMENT                                                    {$$ = createRet($2);}
            | IDENTIFIER ASSIGN expression END_OF_STATEMENT                                         {$$ = createAsgn(createVar($1), $3);}
            | block                                                                                 {$$ = $1;}
            | expression END_OF_STATEMENT                                                           {$$ = $1;}
            ;

call        : IDENTIFIER LEFT_PARENTHESIS expression RIGHT_PARENTHESIS {$$ = createCall($1, $3);}
            | IDENTIFIER LEFT_PARENTHESIS RIGHT_PARENTHESIS            {$$ = createCall($1);}
            | PRINT LEFT_PARENTHESIS expression RIGHT_PARENTHESIS      {$$ = createCall("print", $3);}
            | READ LEFT_PARENTHESIS RIGHT_PARENTHESIS                  {$$ = createCall("read");}
            ;

extern_print: EXTERN VOID_DECLARATION PRINT LEFT_PARENTHESIS INT_DECLARATION RIGHT_PARENTHESIS END_OF_STATEMENT {$$ = createExtern("print");}
            | EXTERN VOID_DECLARATION PRINT LEFT_PARENTHESIS INT_DECLARATION IDENTIFIER RIGHT_PARENTHESIS END_OF_STATEMENT {$$ = createExtern("print");}
            ;

extern_read : EXTERN INT_DECLARATION READ LEFT_PARENTHESIS RIGHT_PARENTHESIS END_OF_STATEMENT {$$ = createExtern("read");}
            ;

func        : INT_DECLARATION IDENTIFIER LEFT_PARENTHESIS param_func_possibilities RIGHT_PARENTHESIS block   {$$ = createFunc($2, $4, $6);}
            | VOID_DECLARATION IDENTIFIER LEFT_PARENTHESIS param_func_possibilities RIGHT_PARENTHESIS block  {$$ = createFunc($2, $4, $6);}
            ;



param_func_possibilities       :                              {$$ = NULL;}
                               | INT_DECLARATION IDENTIFIER   {$$ = createVar($2);}
                               ;

stmt_list   : stmt_list stmt        {$1->push_back($2); $$ = $1;}
            |                       {$$ = new std::vector<astNode*>();}
            ;

block       : LEFT_CURLY stmt_list RIGHT_CURLY      {$$ = createBlock($2);}
            ;

%%

// main removed as main_function now handles all execution

int semantic_analysis(astNode* root){
    scopes.clear();
    get_into_scope();
    int err = traverse(root); // returns 1 if error and 0 if matches the semantic rules
    get_out_of_scope();
    return err;
}
// Depth First Search to traverse the AST
int traverse(astNode* node){
    if (node == NULL) return 0;

    switch (node->type){
        case (ast_prog) :
            return traverse(node->prog.func);
        
        case (ast_func) : {
            get_into_scope();
            if (node -> func.param != NULL) {
                if (declare(node->func.param->var.name)) {
                    get_out_of_scope();
                    return 1;
                }
            }

            astNode* body = node->func.body;

            // This is with the purpose of inspecting the list of the
            // block of the function while being in the same scope. 
            // So that when a variable is
            // declared as parameter, and in the body of the function it is also declared
            // that generates a semantic error by being in the same scope

            
            if (body && body->type == ast_stmt && body->stmt.type == ast_block){
                int err = traverse_stmt_list(body->stmt.block.stmt_list);
                get_out_of_scope();
                return err;
            }
            
            int err = traverse(body);
            get_out_of_scope();
            return err;
        }

        case (ast_extern) :
            return 0; // externs, as defined, cannot break the rules

        case (ast_var) :
            // detect case for when a variable is being used without
            // being declared
            if (!declared_in_any_scope(node->var.name)) {
                semantic_error_notifier("You tried using a var that was not been declared, namely", node->var.name);
                return 1;
            };
            return 0;

        case (ast_cnst) :
            return 0; // constants cannot break the rules

        case (ast_stmt) :
            switch (node->stmt.type) {
                case (ast_call) :
                    if (node->stmt.call.param != NULL) {
                        // to check if the potential variables follow the rules
                        return traverse(node->stmt.call.param); 
                    }
                    return 0;
                
                case (ast_ret) :
                    if (node -> stmt.ret.expr != NULL) {
                        return traverse(node->stmt.ret.expr);
                    }
                    return 0; // no rule has been broken
                                
                case (ast_block) : {
                    get_into_scope();
                    std::vector<astNode*>* stmts = node -> stmt.block.stmt_list;
                    // the function serves to iterate over each stmt and if semantic 
                    // error found 1 is returned otherwise 0 is
                    int err = traverse_stmt_list(stmts);
                    get_out_of_scope();
                    return err;
                }
                                
                case (ast_while) :
                    if (node -> stmt.whilen.cond != NULL) {
                        if (traverse(node->stmt.whilen.cond)) return 1;
                    }
                    if (node -> stmt.whilen.body != NULL) {
                        if (traverse(node->stmt.whilen.body)) return 1;
                    }
                    return 0;
                                
                case (ast_if) :
                    if (node -> stmt.ifn.cond != NULL) {
                        if (traverse(node->stmt.ifn.cond)) return 1;
                    }
                    if (node -> stmt.ifn.if_body != NULL) {
                        if (traverse(node->stmt.ifn.if_body)) return 1;
                    } 
                    if (node -> stmt.ifn.else_body != NULL) {
                        if (traverse(node->stmt.ifn.else_body)) return 1;
                    } 
                    return 0;
                                
                case (ast_decl) :
                    // check b/c we can only have one declaration per variable in the scope 
                    return (declare(node->stmt.decl.name));
                                
                case (ast_asgn) :
                    if (node -> stmt.asgn.lhs != NULL){
                        // because the identifier associated with an assignment is
                        // declared as a var using createVar in the parsing stage
                        // and we handle that case in this function
                        if(traverse(node->stmt.asgn.lhs)) return 1;
                    }
                    if (node -> stmt.asgn.rhs != NULL){
                        // it is an expression thus traversing is also reasonable
                        if(traverse(node->stmt.asgn.rhs)) return 1;
                    }
                    return 0;

                default:
                    fprintf(stderr, "Unknown AST statement type\n"); // since other cases are not considered
                    return 1; 
            }

        case (ast_rexpr) :
            if (node->rexpr.lhs != NULL){
                if (traverse(node->rexpr.lhs)) return 1;
            }

            if (node->rexpr.rhs != NULL){
                if (traverse(node->rexpr.rhs)) return 1;
            }
            
            return 0;

        case (ast_bexpr) :
            if (node->bexpr.lhs != NULL) {
                if (traverse(node->bexpr.lhs)) return 1;
            } 

            if (node->bexpr.rhs != NULL) {
                if (traverse(node->bexpr.rhs)) return 1;
            }
            
            return 0;

        case (ast_uexpr) :
            if (node->uexpr.expr != NULL) return traverse(node->uexpr.expr);
            fprintf(stderr, "Unary expression does not have an operand. Error.\n");
            return 1;

        default:
            fprintf(stderr, "Unknown AST node type\n");
            return 1; // since other cases are not considered
    }
}

void get_into_scope(){
    scopes.push_back({}); // initializes an empty unordered set to read from the current scope
}

void get_out_of_scope(){
    if (!scopes.empty()) scopes.pop_back(); // the innermost symbol table was inspected as now we go to the outer one
}

int declare(const char* name){
    // include only if there is no current declaration
    // to enforce semantic rule only one declaration per variable in scope
    if (has_declaration_in_scope(name)) { 
        semantic_error_notifier("In a scope there cannot be double declaration of the same variable", name);
       return 1; 
    }

    scopes.back().insert(std::string(name));
    
    return 0;
}

// to emphasize two rules better
int semantic_error_notifier(const char* message, const char* name_of_associated_var) {
    fprintf(stderr, "Semantic error: %s '%s'\n", message, name_of_associated_var);
    return 1;
}

int traverse_stmt_list(std::vector<astNode*>* stmt_list){
    if (!stmt_list) return 0;
    for (astNode* stmt : *stmt_list){
        if (traverse(stmt)) return 1;
    }
    return 0;
}

int declared_in_any_scope(const char* name){
    // cast to integer so that 'i' does not behave like an unsigned integer and lead to indexing problems.
    for (int i = ((int) scopes.size())-1; i >= 0; i--){
        if (scopes[i].count(std::string(name))  > 0) return 1;
    }
    return 0;
}

int has_declaration_in_scope(const char* name){
    assert(!scopes.empty()); // just to guarantee the following operation is valid
    return scopes.back().count(std::string(name)) > 0;
}

void yyerror (const char *s) {fprintf(stderr, "%s\n", s);}
