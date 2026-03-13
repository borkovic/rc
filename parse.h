/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_PARSE_TAB_H_INCLUDED
# define YY_YY_PARSE_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    ANDAND = 258,                  /* ANDAND  */
    BACKBACK = 259,                /* BACKBACK  */
    BANG = 260,                    /* BANG  */
    CASE = 261,                    /* CASE  */
    COUNT = 262,                   /* COUNT  */
    DUP = 263,                     /* DUP  */
    ELSE = 264,                    /* ELSE  */
    END = 265,                     /* END  */
    FLAT = 266,                    /* FLAT  */
    FN = 267,                      /* FN  */
    FOR = 268,                     /* FOR  */
    IF = 269,                      /* IF  */
    IN = 270,                      /* IN  */
    OROR = 271,                    /* OROR  */
    PIPE = 272,                    /* PIPE  */
    REDIR = 273,                   /* REDIR  */
    SREDIR = 274,                  /* SREDIR  */
    SUB = 275,                     /* SUB  */
    SUBSHELL = 276,                /* SUBSHELL  */
    SWITCH = 277,                  /* SWITCH  */
    TWIDDLE = 278,                 /* TWIDDLE  */
    WHILE = 279,                   /* WHILE  */
    WORD = 280,                    /* WORD  */
    HUH = 281,                     /* HUH  */
    PREDIR = 282                   /* PREDIR  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif
/* Token kinds.  */
#define YYEMPTY -2
#define YYEOF 0
#define YYerror 256
#define YYUNDEF 257
#define ANDAND 258
#define BACKBACK 259
#define BANG 260
#define CASE 261
#define COUNT 262
#define DUP 263
#define ELSE 264
#define END 265
#define FLAT 266
#define FN 267
#define FOR 268
#define IF 269
#define IN 270
#define OROR 271
#define PIPE 272
#define REDIR 273
#define SREDIR 274
#define SUB 275
#define SUBSHELL 276
#define SWITCH 277
#define TWIDDLE 278
#define WHILE 279
#define WORD 280
#define HUH 281
#define PREDIR 282

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 31 "parse.y"

	struct Node *node;
	struct Redir redir;
	struct Pipe pipe;
	struct Dup dup;
	struct Word word;
	char *keyword;

#line 130 "parse.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_PARSE_TAB_H_INCLUDED  */
