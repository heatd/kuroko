#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "memory.h"

#include "rline.h"

static KrkValue _krk_repl_help(int argc, KrkValue argv[]) {
	fprintf(stderr, "Kuroko REPL\n");
	fprintf(stderr, "Statements entered in the repl will be interpreted directly\n");
	fprintf(stderr, "in a script context and results will be automatically printed.\n");
	fprintf(stderr, "Classes, functions, and control flow statements may also be\n");
	fprintf(stderr, "entered. When in an indented block context, entering a blank\n");
	fprintf(stderr, "line will mark the end of the top-level statement.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Some things to try:\n");
	fprintf(stderr, "   Basic mathematics: 1 + 2 + 3\n");
	fprintf(stderr, "   Import modules: import system\n");
	fprintf(stderr, "   Default a function: def method(foo):\n");
	fprintf(stderr, "                           print foo\n");
	fprintf(stderr, "\n");
	return NONE_VAL();
}

int main(int argc, char * argv[]) {
	krk_initVM();

	int opt;
	while ((opt = getopt(argc, argv, "tdgs")) != -1) {
		switch (opt) {
			case 't':
				vm.enableTracing = 1;
				break;
			case 'd':
				vm.enableDebugging = 1;
				break;
			case 's':
				vm.enableScanTracing = 1;
				break;
			case 'g':
				vm.enableStressGC = 1;
				break;
		}
	}

	KrkValue result = INTEGER_VAL(0);

	if (optind == argc) {
		/* Run the repl */
		int exit = 0;

		/* Bind help() command */
		krk_defineNative(&vm.globals, "help", _krk_repl_help);

		rline_exit_string="";
		rline_exp_set_syntax("python");
		//rline_exp_set_shell_commands(shell_commands, shell_commands_len);
		//rline_exp_set_tab_complete_func(tab_complete_func);

		while (!exit) {
			size_t lineCapacity = 8;
			size_t lineCount = 0;
			char ** lines = ALLOCATE(char *, lineCapacity);
			size_t totalData = 0;
			int valid = 1;
			char * allData = NULL;
			int inBlock = 0;
			int blockWidth = 0;

			rline_exp_set_prompts(">>> ", "", 4, 0);

			while (1) {
				/* This would be a nice place for line editing */
				char buf[4096] = {0};

				if (inBlock) {
					rline_exp_set_prompts("  > ", "", 4, 0);
					rline_preload = malloc(blockWidth + 1);
					for (int i = 0; i < blockWidth; ++i) {
						rline_preload[i] = ' ';
					}
					rline_preload[blockWidth] = '\0';
				}

				rline_scroll = 0;
				if (rline(buf, 4096) == 0) {
					valid = 0;
					exit = 1;
					break;
				}
				if (buf[strlen(buf)-1] != '\n') {
					fprintf(stderr, "Expected end of line in repl input. Did you ^D early?\n");
					valid = 0;
					break;
				}
				if (lineCapacity < lineCount + 1) {
					size_t old = lineCapacity;
					lineCapacity = GROW_CAPACITY(old);
					lines = GROW_ARRAY(char *,lines,old,lineCapacity);
				}

				int i = lineCount++;
				lines[i] = strdup(buf);

				size_t lineLength = strlen(lines[i]);
				totalData += lineLength;

				int is_spaces = 1;
				int count_spaces = 0;
				for (size_t j = 0; j < lineLength; ++j) {
					if (lines[i][j] != ' ' && lines[i][j] != '\n') {
						is_spaces = 0;
						break;
					}
					count_spaces += 1;
				}

				if (lineLength > 2 && lines[i][lineLength-2] == ':') {
					inBlock = 1;
					blockWidth = count_spaces + 4;
					continue;
				} else if (inBlock && lineLength != 1) {
					if (is_spaces) {
						free(lines[i]);
						totalData -= lineLength;
						lineCount--;
						break;
					}
					blockWidth = count_spaces;
					continue;
				}

				break;
			}

			if (valid) {
				allData = malloc(totalData + 1);
				allData[0] = '\0';
			}
			for (size_t i = 0; i < lineCount; ++i) {
				if (valid) strcat(allData, lines[i]);
				rline_history_insert(strdup(lines[i]));
				free(lines[i]);
			}
			FREE_ARRAY(char *, lines, lineCapacity);

			if (valid) {
				KrkValue result = krk_interpret(allData, 0, "<module>","<stdin>");
				if (!IS_NONE(result)) {
					fprintf(stdout, " \033[1;30m=> ");
					krk_printValue(stdout, result);
					fprintf(stdout, "\033[0m\n");
				}
			}

		}
	} else {

		for (int i = optind; i < argc; ++i) {
			KrkValue out = krk_runfile(argv[i],0,"<module>",argv[i]);
			if (i + 1 == argc) result = out;
		}
	}

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
