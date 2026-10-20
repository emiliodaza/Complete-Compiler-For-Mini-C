%{
    #include "parser_and_analyzer.tab.h"
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <errno.h>
    #include <limits.h>
%}
%option yylineno noinput nounput
%x COMMENT
%%
"//"[^\n]*                  ;
"/*"                        {BEGIN(COMMENT);}
<COMMENT>"*/"               {BEGIN(INITIAL);}
<COMMENT>\n                 ;
<COMMENT>.                  ;
<COMMENT><<EOF>>            {fprintf(stderr, "Unterminated comment at line %d\n", yylineno); return YYUNDEF;}
[0-9]+                      {errno = 0; long value = strtol(yytext, NULL, 10); if (errno || value > INT_MAX) {fprintf(stderr, "Integer literal out of range at line %d\n", yylineno); return YYUNDEF;} yylval.ival = (int)value; return NUM;}
"+"                         {return ADD;}
"-"                         {return MINUS;}
"/"                         {return DIVIDE;}
"*"                         {return MUL;}

"<="                        {return LE;}
">="                        {return GE;}
"=="                        {return EQ;}
"!="                        {return NEQ;}
"<"                         {return LT;}
">"                         {return GT;}

"="                         {return ASSIGN;}

"while"                     {return WHILE;}
"("                         {return LEFT_PARENTHESIS;}
")"                         {return RIGHT_PARENTHESIS;}
"{"                         {return LEFT_CURLY;}
"}"                         {return RIGHT_CURLY;}

"if"                        {return IF;}
"else"                      {return ELSE;}

"print"                     {return PRINT;}
"read"                      {return READ;}
"return"                    {return RETURN;}

";"                         {return END_OF_STATEMENT;}

"extern"                    {return EXTERN;}
"int"                       {return INT_DECLARATION;} 
"void"                      {return VOID_DECLARATION;}

[a-zA-Z_][a-zA-Z0-9_]*      {yylval.sval = strdup(yytext); return IDENTIFIER;}

[ \t\r\n]+                  ;

.                           {fprintf(stderr, "Invalid syntax error '%s'\n", yytext); exit(1);}
%%

int yywrap(void){return 1;}
