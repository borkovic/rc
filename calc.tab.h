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

#ifndef YY_CALC_CALC_TAB_H_INCLUDED
# define YY_CALC_CALC_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int calcdebug;
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
    CALC_OROR = 258,               /* CALC_OROR  */
    CALC_ANDAND = 259,             /* CALC_ANDAND  */
    CALC_EQEQ = 260,               /* CALC_EQEQ  */
    CALC_NEQ = 261,                /* CALC_NEQ  */
    CALC_LEQ = 262,                /* CALC_LEQ  */
    CALC_GEQ = 263,                /* CALC_GEQ  */
    CALC_LSHIFT = 264,             /* CALC_LSHIFT  */
    CALC_RSHIFT = 265,             /* CALC_RSHIFT  */
    CALC_UNARY_PLUSMINUS = 266,    /* CALC_UNARY_PLUSMINUS  */
    CALC_NUMBER = 267,             /* CALC_NUMBER  */
    CALC_VAR = 268,                /* CALC_VAR  */
    CALC_END_TOKEN = 269,          /* CALC_END_TOKEN  */
    CALC_BAD_TOKEN = 270           /* CALC_BAD_TOKEN  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 40 "calc.y"

	CalcValue m_Val;

#line 83 "calc.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE calclval;


int calcparse (void);


#endif /* !YY_CALC_CALC_TAB_H_INCLUDED  */
