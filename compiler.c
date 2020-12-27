#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kuroko.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"
#include "object.h"
#include "debug.h"

typedef struct {
	KrkToken current;
	KrkToken previous;
	int hadError;
	int panicMode;
} Parser;

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT, /* = */
	PREC_OR,         /* or */
	PREC_AND,        /* and */
	PREC_EQUALITY,   /* == != in */
	PREC_COMPARISON, /* < > <= >= */
	PREC_TERM,       /* + - */
	PREC_FACTOR,     /* * */
	PREC_UNARY,      /* ! - not */
	PREC_CALL,       /* . () */
	PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(int);

typedef struct {
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;

typedef struct {
	KrkToken name;
	ssize_t depth;
	int isCaptured;
} Local;

typedef struct {
	size_t index;
	int    isLocal;
} Upvalue;

typedef enum {
	TYPE_FUNCTION,
	TYPE_MODULE,
} FunctionType;

#define MAX_LOCALS 256
typedef struct Compiler {
	struct Compiler * enclosing;
	KrkFunction * function;
	FunctionType type;
	Local  locals[MAX_LOCALS];
	size_t localCount;
	size_t scopeDepth;
	Upvalue upvalues[MAX_LOCALS];
} Compiler;

Parser parser;
Compiler * current = NULL;

static KrkChunk * currentChunk() {
	return &current->function->chunk;
}

#define EMIT_CONSTANT_OP(opc, arg) do { if (arg < 256) { emitBytes(opc, arg); } \
	else { emitBytes(opc ## _LONG, arg >> 16); emitBytes(arg >> 8, arg); } } while (0)

static void initCompiler(Compiler * compiler, FunctionType type) {
	compiler->enclosing = current;
	compiler->function = NULL;
	compiler->type = type;
	compiler->localCount = 0;
	compiler->scopeDepth = 0;
	compiler->function = newFunction();
	current = compiler;

	if (type != TYPE_MODULE) {
		current->function->name = copyString(parser.previous.start, parser.previous.length);
	}

	Local * local = &current->locals[current->localCount++];
	local->depth = 0;
	local->name.start = "";
	local->name.length = 0;
	local->isCaptured = 0;
}

static void parsePrecedence(Precedence precedence);
static ssize_t parseVariable(const char * errorMessage);
static void defineVariable(size_t global);
static uint8_t argumentList();
static ssize_t identifierConstant(KrkToken * name);
static ssize_t resolveLocal(Compiler * compiler, KrkToken * name);
static ParseRule * getRule(KrkTokenType type);
static void defDeclaration();
static void expression();
static void statement();
static void declaration();
static void or_(int canAssign);
static void and_(int canAssign);
static void classDeclaration();
static void declareVariable();

static void errorAt(KrkToken * token, const char * message) {
	if (parser.panicMode) return;
	parser.panicMode = 1;

	fprintf(stderr, "[line %d] Error", (int)token->line);
	if (token->type == TOKEN_EOF) {
		fprintf(stderr, " at end");
	} else if (token->type != TOKEN_ERROR) {
		fprintf(stderr, " at '%.*s'", (int)token->length, token->start);
	}

	fprintf(stderr, ": %s\n", message);
	parser.hadError = 1;
}

static void error(const char * message) {
	errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char * message) {
	errorAt(&parser.previous, "(token before actual error)");
	parser.panicMode = 0;
	errorAt(&parser.current, message);
}

static void advance() {
	parser.previous = parser.current;

	for (;;) {
		parser.current = krk_scanToken();
		if (parser.current.type == TOKEN_RETRY) continue;
		if (parser.current.type != TOKEN_ERROR) break;

		errorAtCurrent(parser.current.start);
	}
}

static void consume(KrkTokenType type, const char * message) {
	if (parser.current.type == type) {
		advance();
		return;
	}

	errorAtCurrent(message);
}

static int check(KrkTokenType type) {
	return parser.current.type == type;
}

static int match(KrkTokenType type) {
	if (!check(type)) return 0;
	advance();
	return 1;
}

static void emitByte(uint8_t byte) {
	krk_writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

static void emitReturn() {
	emitBytes(OP_NONE, OP_RETURN);
}

static KrkFunction * endCompiler() {
	emitReturn();
	KrkFunction * function = current->function;
#define DEBUG
#ifdef DEBUG
	if (!parser.hadError) {
		krk_disassembleChunk(currentChunk(), function->name != NULL ? function->name->chars : "<module>");
	}
#endif

	current = current->enclosing;
	return function;
}

static void endOfLine() {
	if (!(match(TOKEN_EOL) || match(TOKEN_EOF))) {
		errorAtCurrent("Expected end of line.");
	}
}

static size_t emitConstant(KrkValue value) {
	return krk_writeConstant(currentChunk(), value, parser.previous.line);
}

static void number(int canAssign) {
	const char * start = parser.previous.start;
	int base = 10;

	/*  These special cases for hexadecimal, binary, octal values. */
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		base = 16;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
		base = 2;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'o' || start[1] == 'O')) {
		base = 8;
		start += 2;
	}

	/* If it wasn't a special base, it may be a floating point value. */
	if (base == 10) {
		for (size_t j = 0; j < parser.previous.length; ++j) {
			if (parser.previous.start[j] == '.') {
				double value = strtod(start, NULL);
				emitConstant(FLOATING_VAL(value));
				return;
			}
		}
	}

	/* If we got here, it's an integer of some sort. */
	int value = strtol(start, NULL, base);
	emitConstant(INTEGER_VAL(value));
}

static void binary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;
	ParseRule * rule = getRule(operatorType);
	parsePrecedence((Precedence)(rule->precedence + 1));

	switch (operatorType) {
		case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
		case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
		case TOKEN_GREATER:       emitByte(OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
		case TOKEN_LESS:          emitByte(OP_LESS); break;
		case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;

		case TOKEN_PLUS:     emitByte(OP_ADD); break;
		case TOKEN_MINUS:    emitByte(OP_SUBTRACT); break;
		case TOKEN_ASTERISK: emitByte(OP_MULTIPLY); break;
		case TOKEN_SOLIDUS:  emitByte(OP_DIVIDE); break;
		default: return;
	}
}

static void call(int canAssign) {
	uint8_t argCount = argumentList();
	emitBytes(OP_CALL, argCount);
}

static void dot(int canAssign) {
	consume(TOKEN_IDENTIFIER, "Expected propert name");
	size_t ind = identifierConstant(&parser.previous);
	if (canAssign && match(TOKEN_EQUAL)) {
		expression();
		EMIT_CONSTANT_OP(OP_SET_PROPERTY, ind);
	} else {
		EMIT_CONSTANT_OP(OP_GET_PROPERTY, ind);
	}
}

static void literal(int canAssign) {
	switch (parser.previous.type) {
		case TOKEN_FALSE: emitByte(OP_FALSE); break;
		case TOKEN_NONE:  emitByte(OP_NONE); break;
		case TOKEN_TRUE:  emitByte(OP_TRUE); break;
		default: return;
	}
}

static void expression() {
	parsePrecedence(PREC_ASSIGNMENT);
}

static void varDeclaration() {
	ssize_t global = parseVariable("Expected variable name.");

	if (match(TOKEN_EQUAL)) {
		expression();
	} else {
		emitByte(OP_NONE);
	}

	defineVariable(global);
}

static void printStatement() {
	expression();
	emitByte(OP_PRINT);
}

static void synchronize() {
	parser.panicMode = 0;
	while (parser.current.type != TOKEN_EOF) {
		if (parser.previous.type == TOKEN_EOL) return;

		switch (parser.current.type) {
			case TOKEN_CLASS:
			case TOKEN_DEF:
			case TOKEN_LET:
			case TOKEN_FOR:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_PRINT:
			case TOKEN_RETURN:
				return;
			default: break;
		}

		advance();
	}
}

static void declaration() {
	if (check(TOKEN_DEF)) {
		defDeclaration();
	} else if (match(TOKEN_LET)) {
		varDeclaration();
	} else if (check(TOKEN_CLASS)) {
		classDeclaration();
	} else if (check(TOKEN_EOL)) {
		return;
	} else {
		statement();
	}

	if (parser.panicMode) synchronize();
}

static void expressionStatement() {
	expression();
	emitByte(OP_POP);
}

static void beginScope() {
	current->scopeDepth++;
}

static void endScope() {
	current->scopeDepth--;
	while (current->localCount > 0 &&
	       current->locals[current->localCount - 1].depth > current->scopeDepth) {
		if (current->locals[current->localCount - 1].isCaptured) {
			emitByte(OP_CLOSE_UPVALUE);
		} else {
			emitByte(OP_POP);
		}
		current->localCount--;
	}
}

static void block(int indentation) {
	if (match(TOKEN_EOL)) {
		/* Begin actual blocks */
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = parser.current.length;
			if (currentIndentation <= indentation) {
				errorAtCurrent("Unexpected indentation level for new block");
			}
			do {
				if (parser.current.length != currentIndentation) break;
				advance();
				declaration();
				if (check(TOKEN_EOL)) endOfLine();
			} while (check(TOKEN_INDENTATION));
			//fprintf(stderr, "Exiting from block %d to indentation level %d (line %d)\n",
			//	(int)currentIndentation, check(TOKEN_INDENTATION) ? (int)parser.current.length : 0, (int)parser.previous.line);
		} else {
			errorAtCurrent("Expected indentation for block");
		}
	} else {
		errorAtCurrent("Unsupported single-line block");
	}
}

static void function(FunctionType type, int blockWidth) {
	Compiler compiler;
	initCompiler(&compiler, type);

	beginScope();

	consume(TOKEN_LEFT_PAREN, "Expected start of parameter list after function name.");
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			current->function->arity++;
			if (current->function->arity > 255) errorAtCurrent("too many function parameters");
			ssize_t paramConstant = parseVariable("Expect parameter name.");
			defineVariable(paramConstant);
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expected end of parameter list.");

	consume(TOKEN_COLON, "Expected colon after function signature.");
	block(blockWidth);

	KrkFunction * function = endCompiler();
	size_t ind = krk_addConstant(currentChunk(), OBJECT_VAL(function));
	EMIT_CONSTANT_OP(OP_CLOSURE, ind);

	for (size_t i = 0; i < function->upvalueCount; ++i) {
		/* TODO: if the maximum count changes, fix the sizes for this */
		emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
		emitByte(compiler.upvalues[i].index);
	}
}

static void classDeclaration() {
	int blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `class` */

	consume(TOKEN_IDENTIFIER, "Expected class name.");
	size_t constInd = identifierConstant(&parser.previous);
	declareVariable();

	EMIT_CONSTANT_OP(OP_CLASS, constInd);
	defineVariable(constInd);

	consume(TOKEN_COLON, "Expected colon after class");
	/* TODO block semantics */
}

static void markInitialized() {
	if (current->scopeDepth == 0) return;
	current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defDeclaration() {
	int blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `def` */

	ssize_t global = parseVariable("Expected function name.");
	markInitialized();
	function(TYPE_FUNCTION, blockWidth);
	defineVariable(global);
}

static int emitJump(uint8_t opcode) {
	emitByte(opcode);
	emitBytes(0xFF, 0xFF);
	return currentChunk()->count - 2;
}

static void patchJump(int offset) {
	int jump = currentChunk()->count - offset - 2;
	if (jump > 0xFFFF) {
		error("Unsupported far jump (we'll get there)");
	}

	currentChunk()->code[offset] = (jump >> 8) & 0xFF;
	currentChunk()->code[offset + 1] =  (jump) & 0xFF;
}

static void emitLoop(int loopStart) {
	emitByte(OP_LOOP);
	int offset = currentChunk()->count - loopStart + 2;
	if (offset > 0xFFFF) error("offset too big");
	emitBytes(offset >> 8, offset);
}

static void ifStatement() {
	/* Figure out what block level contains us so we can match our partner else */
	int blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;

	/* Collect the if token that started this statement */
	advance();

	/* Collect condition expression */
	expression();

	/* if EXPR: */
	consume(TOKEN_COLON, "Expect ':' after condition.");

	int thenJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);

	/* Start a new scope and enter a block */
	beginScope();
	block(blockWidth);
	endScope();

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);
	emitByte(OP_POP);

	/* See if we have a matching else block */
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		if (blockWidth) advance();
		if (match(TOKEN_ELSE)) {
			/* TODO ELIF or ELSE IF */
			consume(TOKEN_COLON, "Expect ':' after else.");
			beginScope();
			block(blockWidth);
			endScope();
		}
	}

	patchJump(elseJump);
}

static void whileStatement() {
	int blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	int loopStart = currentChunk()->count;

	expression();
	consume(TOKEN_COLON, "Expect ':' after condition.");

	int exitJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);

	beginScope();
	block(blockWidth);
	endScope();

	emitLoop(loopStart);

	patchJump(exitJump);
	emitByte(OP_POP);
}

static void forStatement() {
	/* I'm not sure if I want this to be more like Python or C/Lox/etc. */
	int blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	/* For now this is going to be kinda broken */
	beginScope();

	/* actually should be `for NAME in ITERABLE` ... */
	varDeclaration();

	consume(TOKEN_COMMA,"expect ,");

	int loopStart = currentChunk()->count;
	expression(); /* condition */
	int exitJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);

	if (check(TOKEN_COMMA)) {
		advance();
		int bodyJump = emitJump(OP_JUMP);
		int incrementStart = currentChunk()->count;
		expression();
		emitByte(OP_POP);

		emitLoop(loopStart);
		loopStart = incrementStart;
		patchJump(bodyJump);
	}

	consume(TOKEN_COLON,"expect :");

	block(blockWidth);

	emitLoop(loopStart);
	patchJump(exitJump);
	emitByte(OP_POP);
	endScope();
}

static void returnStatement() {
	if (check(TOKEN_EOL) || check(TOKEN_EOF)) {
		emitReturn();
	} else {
		expression();
		emitByte(OP_RETURN);
	}
}

static void statement() {
	if (check(TOKEN_EOL)) {
		return; /* Meaningless blank line */
	}

	if (match(TOKEN_PRINT)) {
		printStatement();
	} else if (check(TOKEN_IF)) {
		/*
		 * We check rather than match because we need to look at the indentation
		 * token that came before this (if it was one) to figure out what block
		 * indentation level we're at, so that we can match our companion else
		 * and make sure it's not the else for a higher if block.
		 *
		 * TODO: Are there other things where we want to do this?
		 */
		ifStatement();
	} else if (check(TOKEN_WHILE)) {
		whileStatement();
	} else if (check(TOKEN_FOR)) {
		forStatement();
	} else if (match(TOKEN_RETURN)) {
		returnStatement();
	} else {
		expressionStatement();
	}
}

static void grouping(int canAssign) {
	expression();
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void unary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;

	parsePrecedence(PREC_UNARY);

	switch (operatorType) {
		case TOKEN_MINUS: emitByte(OP_NEGATE); break;

		/* These are equivalent */
		case TOKEN_BANG:
		case TOKEN_NOT:
			emitByte(OP_NOT);
			break;

		default: return;
	}
}

static void string(int canAssign) {
	/* TODO: needs to handle escape sequences */
	size_t r = emitConstant(OBJECT_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

/* TODO
static void codepoint(int canAssign) {
  // Convert utf8 bytes to single codepoint; error on multiple codepoints.
  // Emit as constant Integer value? Or as separate Codepoint value?
  // The latter could add to strings as utf8 bytes, but compare to
  // Integers as the numerical value...
}
*/

static size_t addUpvalue(Compiler * compiler, ssize_t index, int isLocal) {
	size_t upvalueCount = compiler->function->upvalueCount;
	for (size_t i = 0; i < upvalueCount; ++i) {
		Upvalue * upvalue = &compiler->upvalues[i];
		if (upvalue->index == index && upvalue->isLocal == isLocal) {
			return i;
		}
	}
	if (upvalueCount == MAX_LOCALS) {
		error("Too many closure variables in function.");
		return 0;
	}
	compiler->upvalues[upvalueCount].isLocal = isLocal;
	compiler->upvalues[upvalueCount].index = index;
	return compiler->function->upvalueCount++;
}

static ssize_t resolveUpvalue(Compiler * compiler, KrkToken * name) {
	if (compiler->enclosing == NULL) return -1;
	ssize_t local = resolveLocal(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].isCaptured = 1;
		return addUpvalue(compiler, local, 1);
	}
	ssize_t upvalue = resolveUpvalue(compiler->enclosing, name);
	if (upvalue != -1) {
		return addUpvalue(compiler, upvalue, 0);
	}
	return -1;
}

#define DO_VARIABLE(opset,opget) do { \
	if (canAssign && match(TOKEN_EQUAL)) { \
		expression(); \
		EMIT_CONSTANT_OP(opset, arg); \
	} else { \
		EMIT_CONSTANT_OP(opget, arg); \
	} } while (0)

static void namedVariable(KrkToken name, int canAssign) {
	ssize_t arg = resolveLocal(current, &name);
	if (arg != -1) {
		DO_VARIABLE(OP_SET_LOCAL, OP_GET_LOCAL);
	} else if ((arg = resolveUpvalue(current, &name)) != -1) {
		DO_VARIABLE(OP_SET_UPVALUE, OP_GET_UPVALUE);
	} else {
		arg = identifierConstant(&name);
		DO_VARIABLE(OP_SET_GLOBAL, OP_GET_GLOBAL);
	}
}
#undef DO_VARIABLE

static void variable(int canAssign) {
	namedVariable(parser.previous, canAssign);
}

ParseRule rules[] = {
	[TOKEN_LEFT_PAREN]    = {grouping, call,   PREC_CALL},
	[TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_SQUARE]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RIGHT_SQUARE]  = {NULL,     NULL,   PREC_NONE},
	[TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_DOT]           = {NULL,     dot,    PREC_CALL},
	[TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
	[TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
	[TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SOLIDUS]       = {NULL,     binary, PREC_FACTOR},
	[TOKEN_ASTERISK]      = {NULL,     binary, PREC_FACTOR},
	[TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
	[TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
	[TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
	[TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
	[TOKEN_CODEPOINT]     = {NULL,     NULL,   PREC_NONE}, /* TODO */
	[TOKEN_AND]           = {NULL,     and_,   PREC_AND},
	[TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
	[TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
	[TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_DEF]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_IN]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LET]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_NONE]          = {literal,  NULL,   PREC_NONE},
	[TOKEN_NOT]           = {unary,    NULL,   PREC_NONE},
	[TOKEN_OR]            = {NULL,     or_,    PREC_OR},
	[TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SELF]          = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
	[TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},

	/* This is going to get interesting */
	[TOKEN_INDENTATION]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
	advance();
	ParseFn prefixRule = getRule(parser.previous.type)->prefix;
	if (prefixRule == NULL) {
		error("expect expression");
		return;
	}
	int canAssign = precedence <= PREC_ASSIGNMENT;
	prefixRule(canAssign);
	while (precedence <= getRule(parser.current.type)->precedence) {
		advance();
		ParseFn infixRule = getRule(parser.previous.type)->infix;
		infixRule(canAssign);
	}

	if (canAssign && match(TOKEN_EQUAL)) {
		error("invalid assignment target");
	}
}

static ssize_t identifierConstant(KrkToken * name) {
	return krk_addConstant(currentChunk(), OBJECT_VAL(copyString(name->start, name->length)));
}

static int identifiersEqual(KrkToken * a, KrkToken * b) {
	return (a->length == b->length && memcmp(a->start, b->start, a->length) == 0);
}

static ssize_t resolveLocal(Compiler * compiler, KrkToken * name) {
	for (ssize_t i = compiler->localCount - 1; i >= 0; i--) {
		Local * local = &compiler->locals[i];
		if (identifiersEqual(name, &local->name)) {
			if (local->depth == -1) {
				error("can not initialize value recursively (are you shadowing something?)");
			}
			return i;
		}
	}
	return -1;
}

static void addLocal(KrkToken name) {
	if (current->localCount == MAX_LOCALS) {
		error("too many locals");
		return;
	}
	Local * local = &current->locals[current->localCount++];
	local->name = name;
	local->depth = -1;
	local->isCaptured = 0;
}

static void declareVariable() {
	if (current->scopeDepth == 0) return;
	KrkToken * name = &parser.previous;
	/* Detect duplicate definition */
	for (ssize_t i = current->localCount - 1; i >= 0; i--) {
		Local * local = &current->locals[i];
		if (local->depth != -1 && local->depth < current->scopeDepth) break;
		if (identifiersEqual(name, &local->name)) error("Duplicate definition");
	}
	addLocal(*name);
}

static ssize_t parseVariable(const char * errorMessage) {
	consume(TOKEN_IDENTIFIER, errorMessage);

	declareVariable();
	if (current->scopeDepth > 0) return 0;

	return identifierConstant(&parser.previous);
}

static void defineVariable(size_t global) {
	if (current->scopeDepth > 0) {
		markInitialized();
		return;
	}

	EMIT_CONSTANT_OP(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
	uint8_t argCount = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			expression();
			if (argCount == 255) error("Too many arguments to function."); // Need long call...
			argCount++;
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
	return argCount;
}

static void and_(int canAssign) {
	int endJump = emitJump(OP_JUMP_IF_FALSE);
	emitByte(OP_POP);
	parsePrecedence(PREC_AND);
	patchJump(endJump);
}

static void or_(int canAssign) {
	int endJump = emitJump(OP_JUMP_IF_TRUE);
	emitByte(OP_POP);
	parsePrecedence(PREC_OR);
	patchJump(endJump);
}

static ParseRule * getRule(KrkTokenType type) {
	return &rules[type];
}

KrkFunction * krk_compile(const char * src) {
	krk_initScanner(src);
	Compiler compiler;
	initCompiler(&compiler, TYPE_MODULE);

	parser.hadError = 0;
	parser.panicMode = 0;

	advance();

	while (!match(TOKEN_EOF)) {
		declaration();
		if (check(TOKEN_EOL)) advance();
	}

	KrkFunction * function = endCompiler();
	return parser.hadError ? NULL : function;
}

void krk_markCompilerRoots() {
	Compiler * compiler = current;
	while (compiler != NULL) {
		krk_markObject((KrkObj*)compiler->function);
		compiler = compiler->enclosing;
	}
}
