#ifndef CALC_DECL_H_INCLUDE
#define CALC_DECL_H_INCLUDE

typedef long int CalcValue;
#include "calc.h"

typedef calctoken CalcToken;

typedef struct CalcLexData {
	char **m_CurrentWord;
	const char *m_CurrentChar;
	char m_Indent[256];
	CalcValue m_CalcResult;
	const char *m_CalcCmdName;
} CalcLexData;

extern int CalcError(const char *s, const CalcLexData *lexData);
extern CalcValue CalcPower(CalcValue a, CalcValue b);

typedef CalcToken (*CalcLexerType)(CalcLexData *lexData, YYSTYPE *calclval);
extern int CalcParser(CalcLexerType lexer, CalcLexData *lexData);

#endif
