#include<string.h>
#include<stdio.h>
#include<stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

// #include <libexplain/execvp.h>  // -lexplain

#define MIN_STRING_SIZE 16
#define PATHBUF_SIZE    4098

#define EOL '\n'

// State of the execution. 
// Any point of program can terminate main loop.
int eval_state;

// Set of childrens that might handle signals
int childs_count;
int *childs;

typedef enum { // token_type
	FILE_OUT,         // >
	FILE_OUT_APPEND,  // >>
	FILE_IN,          // <
	ASYNC,            // &
	CONV,             // |
	STRING,           // Any string object aka "string" or key aka ls
	NONE              // None
} token_type;

typedef struct { // token
	// Type of the token
	token_type type;
	// String value
	char *string;
} token;

typedef struct { // tokenizer_state
	int is_error;
	token tok;
} tokenizer_state;

// Gets next token. 
// Returns 0 if error occurred or EOF, 1 else.
// Due the error occurs, stste->is_error is set to 1.
// WARNING: No convertion of escaped characters to the normal.
// "\\\n" -> "\\\n" instead of "\<lb>"
int get_token(tokenizer_state *state) {
	state->tok.string = NULL;
	state->tok.type = NONE;
	
	int c = getchar();
	
	// Consume all white- characters
	while (1) {
		if (!(c == ' ' || c == '\t' || c == EOF))
			break;
		
		c = getchar();
	}
	
	if (c == EOL)
		return 0;
	
	if (c == EOF)
		return 0;
	
	if (c == '&') {
		state->tok.type = ASYNC;
		return 1;
	}
	
	if (c == '>') {
		c = getchar();
		if (c == '>') {
			state->tok.type = FILE_OUT_APPEND;
			return 1;
		} else
			ungetc(c, stdin);
		state->tok.type = FILE_OUT;
		return 1;
	}
	
	if (c == '>') {
		return 1;
	}
	
	if (c == '<') {
		state->tok.type = FILE_IN;
		return 1;
	}
	
	if (c == '|') {
		state->tok.type = CONV;
		return 1;
	}
	
	// Parsing string sequence
	if (c == '\"') {
		int alloc_size = MIN_STRING_SIZE;
		int str_index = 0;
		char *buf = calloc(alloc_size, sizeof(char));
		
		while (1) {
			c = getchar();
			if (c == EOF || c == EOL) {
				free(buf);
				printf("Unexpected end of string\n");
				return 0;
			}
			if (c == '\"')
				break;
			
			if (str_index >= alloc_size) 
				buf = realloc(buf, (alloc_size <<= 2) * sizeof(char));
			
			buf[str_index++] = c;
		}
		
		
		if (str_index >= alloc_size) 
			buf = realloc(buf, (alloc_size <<= 2) * sizeof(char));
		
		buf[str_index++] = '\0';
		
		state->tok.string = buf;
		state->tok.type = STRING;
		
		return 1;
	} else {
		int alloc_size = MIN_STRING_SIZE;
		int str_index = 1;
		char *buf = calloc(alloc_size, sizeof(char));
		buf[0] = c;o
		
		while (1) {
			c = getchar();
			if (c == EOF) {
				// free(buf);
				// printf("Unexpected end of string\n");
				// return 0;
				break;
			}
			if (c == EOL || c == '<' || c == '>' || c == '&' || c == '|' || c == '"') {
				ungetc(c, stdin);
				break;
			}
			if (c == ' ' || c == '\t')
				break;
			
			if (str_index >= alloc_size) 
				buf = realloc(buf, (alloc_size <<= 2) * sizeof(char));
			
			buf[str_index++] = c;
		}
		
		
		if (str_index >= alloc_size) 
			buf = realloc(buf, (alloc_size <<= 2) * sizeof(char));
		
		buf[str_index++] = '\0';
		
		state->tok.string = buf;
		state->tok.type = STRING;
		
		return 1;
	}
};

token *tokenize() {
	// Last element is NONE
	token *buf = malloc(sizeof(token));
	buf[0].type = NONE;
	int buf_size = 1;
	
	tokenizer_state state = { 0 };
	while (get_token(&state) && !state.is_error) {
		buf = realloc(buf, (buf_size + 1) * sizeof(token));
		buf[buf_size - 1] = state.tok;
		buf[buf_size].type = NONE;
		++buf_size;
		state = (tokenizer_state) { 0 };
	}
	
	if (state.is_error) {
		for (int i = 0;; ++i)
			if (buf[i].type == NONE)
				break;
			else
				free(buf[i].string);
		free(buf);
	}
	
	// Layout: 
	// [tok 0|tok 1| ... |tok N|NULL]
	return buf;
};

typedef enum {
	APPEND,
	WRITE
} fout_mode;

typedef struct {
	char *name;
	int argc;
	char **argv;
	char *fout;
	fout_mode fout_m;
	char *fin;
} exec_dummy;

void eval() {
	childs_count = 0;
	childs = NULL;
	token *comandline = tokenize();
	if (comandline && comandline[0].type != NONE) {
		if (comandline[0].type == STRING && strcmp(comandline[0].string, "exit") == 0) {
			eval_state = 0;
			goto jump_end;
			// printf("Exit handled\n");
		}
		
		/*
		// Debug:
		// print all input line by line.
		for (int i = 0; comandline[i].type != NONE; ++i) {
			if (comandline[i].type == STRING)
				printf("#%d: \"%s\"\n", i, comandline[i].string);
			else {
				switch(comandline[i].type) {
					case FILE_IN:         printf("#%d: <\n", i);  break;
					case FILE_OUT:        printf("#%d: >\n", i);  break;
					case FILE_OUT_APPEND: printf("#%d: <<\n", i); break;
					case CONV:            printf("#%d: |\n", i); break;
					default: break;
				}
			}
		}
		*/
		
		exec_dummy *dummies = NULL;
		int ndummies = 0;
		int index = 0;
		
		int async_exec = 0;
		
		do {
			// <filename> { <arg1>[, argN>] } { < file } { > file | >> file }
			if (comandline[index].type == STRING) {
				dummies = realloc(dummies, ++ndummies * sizeof(exec_dummy));
				dummies[ndummies-1] = (exec_dummy) { 0 };
				
				// Assign name
				dummies[ndummies-1].name = comandline[index++].string;
				
				// Parse list of arguments
				int argc = 0;
				dummies[ndummies-1].argv = calloc(sizeof(char*), 1);
				dummies[ndummies-1].argv = realloc(dummies[ndummies-1].argv, ++argc * sizeof(char*));
				dummies[ndummies-1].argv[argc - 1] = dummies[ndummies-1].name;
				if (dummies[ndummies-1].name[0] == '.' && dummies[ndummies-1].name[1] == '/') {
					//dummies[ndummies-1].name += 2;
					//dummies[ndummies-1].argv[argc - 1] += 2;
				}
				
				while (1) {
					if (comandline[index].type == NONE
						||
						comandline[index].type == FILE_OUT
						||
						comandline[index].type == FILE_OUT_APPEND
						||
						comandline[index].type == FILE_IN
						||
						comandline[index].type == ASYNC
						||
						comandline[index].type == CONV)
					break;
					
					// Append argument
					dummies[ndummies-1].argv = realloc(dummies[ndummies-1].argv, ++argc * sizeof(char*));
					dummies[ndummies-1].argv[argc - 1] = comandline[index].string;
					++index;
				};
				dummies[ndummies-1].argc = argc;
				
				// Insert NULL
				dummies[ndummies-1].argv = realloc(dummies[ndummies-1].argv, ++argc * sizeof(char*));
				dummies[ndummies-1].argv[argc - 1] = NULL;
				
				// Parse modifiers
				while (1) {
					if (comandline[index].type == NONE)
						break;
					if (comandline[index].type == FILE_OUT) {
						++index;
						if (comandline[index].type != STRING) {
							printf("File expected near >\n");
							for (int i = 0; i < ndummies; ++i)
								free(dummies[i].argv);
							free(dummies);
							goto jump_end;
						}
						dummies[ndummies-1].fout = comandline[index].string;
						dummies[ndummies-1].fout_m = WRITE;
						++index;
						continue;
					}
					if (comandline[index].type == FILE_OUT_APPEND) {
						++index;
						if (comandline[index].type != STRING) {
							printf("File expected near >>\n");
							for (int i = 0; i < ndummies; ++i)
								free(dummies[i].argv);
							free(dummies);
							goto jump_end;
						}
						dummies[ndummies-1].fout = comandline[index].string;
						dummies[ndummies-1].fout_m = APPEND;
						++index;
						continue;
					}
					if (comandline[index].type == FILE_IN) {
						++index;
						if (comandline[index].type != STRING) {
							printf("File expected near <\n");
							for (int i = 0; i < ndummies; ++i)
								free(dummies[i].argv);
							free(dummies);
							goto jump_end;
						}
						dummies[ndummies-1].fin = comandline[index].string;
						++index;
						continue;
					}
					if (comandline[index].type == ASYNC) {
						++index;
						async_exec = 1;
						if (comandline[index].type != NONE) {
							printf("EOL expected near &\n");
							for (int i = 0; i < ndummies; ++i)
								free(dummies[i].argv);
							free(dummies);
							goto jump_end;
						}
						break;
					}
					if (comandline[index].type == CONV) 
						break;
				}
				
				if (comandline[index].type == NONE)
					break;
				if (comandline[index].type == ASYNC)
					break;
				
				++index;
			} else {
				printf("String expected\n");
				for (int i = 0; i < ndummies; ++i)
					free(dummies[i].argv);
				free(dummies);
				goto jump_end;
			}
		} while (1);
		
		/*
		// Debug print tasks:
		for (int i = 0; i < ndummies; ++i) {
			printf("#%d\n| name: \"%s\"\n", i, dummies[i].name);
			
			printf("|\n");
			for (int j = 0; j < dummies[i].argc; ++j)
				printf("| argv[%d]: \"%s\"\n", j+1, dummies[i].argv[j]);
			printf("\n");
			if (dummies[i].fout != NULL)
				printf("| fout:  \"%s\", mode: %s\n", dummies[i].fout, (dummies[i].fout_m == APPEND ? "append" : "rewrite"));
			if (dummies[i].fin != NULL)
				printf("| fin:   \"%s\"\n", dummies[i].fin);
		}
		*/
		
		// Execute basing on task type
		if (async_exec) { // Async
			if (fork() == 0) { // Start son
				// printf("Dummy son pid: %d\n", getpid());
				signal(SIGINT, SIG_IGN);
				signal(SIGSTOP, SIG_IGN);
				eval_state = 0;
				
				// Deattach from tty
				int rc = setsid();
				// printf("Child sid set to %d\n");
				
				close(0);
				dup(open("/dev/null", O_RDONLY));
				close(1);
				dup(open("/dev/null", O_WRONLY));
				close(2);
				dup(open("/dev/null", O_WRONLY));
				
				if (ndummies == 1) {
					// childs = malloc(sizeof(int));
					
					if (fork() == 0) { // Child
						// Signals
						signal(SIGINT, SIG_DFL);
						signal(SIGSTOP, SIG_DFL);
						
						// stdin
						if (dummies[0].fin) {
							int fin = open(dummies[0].fin, O_RDONLY);
							
							if (fin == -1) {
								printf("File %s not found.\n", dummies[0].fin);
								eval_state = 0;
								goto jump_send;
							}
							
							// fin -> stdin
							close(0);
							dup(fin);
						}
						
						// stdout
						if (dummies[0].fout) {
							int fout;
							if (dummies[0].fout_m == APPEND)
								fout = open(dummies[0].fout, O_CREAT | O_WRONLY | O_APPEND);
							else
								fout = open(dummies[0].fout, O_CREAT | O_WRONLY);
							
							if (fout == -1) {
								printf("File %s not found.\n", dummies[0].fout);
								eval_state = 0;
								goto jump_send;
							}
							
							// fout -> stdout
							close(1);
							dup(fout);
						}
						
						// exec
						execv(dummies[0].name, dummies[0].argv);
						execvp(dummies[0].name, dummies[0].argv);
						printf("Execution error: File not found.\n");
						// printf("%s\n", explain_execv(dummies[0].name, dummies[0].argv));
						eval_state = 0;
						goto jump_send;
					}
					
					// while (wait(NULL) > 0);
					// childs_count = 0;
					// free(childs);
					// childs = NULL;
					
				} else {
					// Dummies structure:
					// [#1: in: file/stdio, out: #2] | ... | [#k: in: #k-1, out: #k+1] | ... | [#N: in: #N-1, out: file/stdout]
					
					for (int i = 1; i < ndummies - 1; ++i)
						if (dummies[i].fin || dummies[i].fout) {
							// Conveyor entry cant output to a file.
							printf("Conveyor entry cant output to a file.\n");
							goto jump_send;
						}
					if (dummies[0].fout) {
						// Conveyor entry cant output to a file.
						printf("Conveyor entry cant output to a file.\n");
						goto jump_send;
					}
					if (dummies[ndummies-1].fin) {
						// Conveyor entry cant input to a file.
						printf("Conveyor entry cant input to a file.\n");
						goto jump_send;
					}
					
					// childs = malloc(ndummies * sizeof(int));
					// childs_count = ndummies;
					
					int fd1[2] = { -1, -1 }; 
					int fd2[2] = { -1, -1 };
					for (int procind = 0; procind < ndummies; ++procind) {
						fd2[0] = fd1[0];
						fd2[1] = fd1[1];
						
						// 1 -> 2, 2 -> 3, ..., N-1 -> N: total: N-1 pipe
						if (procind < ndummies - 1)
							pipe(fd1);
						
						if (fork() == 0) {
							free(childs); // Release
							// Signals
							signal(SIGINT, SIG_DFL);
							signal(SIGSTOP, SIG_DFL);
							
							if (procind == 0) {
								// printf("Executing %s as primary\n", dummies[procind].name);
								close(1);      // stdout -> pipe::write
								dup(fd1[1]);   // close pipe::write
								close(fd1[0]); // disable pipe::read
								close(fd1[1]); // disable pipe::write
							} else if (procind == ndummies - 1) {
								// printf("Executing %s as final\n", dummies[procind].name);
								close(0);      // stdin -> pipe::read
								dup(fd2[0]);   // close pipe::read
								close(fd2[0]); // disable pipe::read
								close(fd2[1]); // disable pipe::write
							} else {
								// printf("Executing %s as middle\n", dummies[procind].name);
								close(0);      // stdin -> pipe::read
								dup(fd2[0]);   // close pipe::write
								close(fd2[0]); // disable pipe::read
								close(fd2[1]); // disable pipe::write
								close(1);      // stdout -> pipe::write
								dup(fd1[1]);   // close pipe::write
								close(fd1[0]); // disable pipe::read
								close(fd1[1]); // disable pipe::write
							}
						
							// exec
							execv(dummies[procind].name, dummies[procind].argv);
							execvp(dummies[procind].name, dummies[procind].argv);
							printf("Execution error: File not found.\n");
							eval_state = 0;
							goto jump_send;
						}
						
						// Close read/write after fork
						if (fd2[0] != -1) 
							close(fd2[0]);
						if (fd2[1] != -1)
							close(fd2[1]);
					} 
					
					// Wait for all childs
					// while (wait(NULL) > 0);
					// childs_count = 0;
					// free(childs);
					// childs = NULL;
				}
				
				// No wait for childs to die
				// while (wait(NULL) > 0);
				goto jump_send;
			}
			
			// printf("Waiting for dummy son to die\n");
			while (wait(NULL) > 0);
			// printf("Dummy son dead\n");
		} else { // Sync
			if (ndummies == 1) {
				childs = malloc(sizeof(int));
				
				if ((childs[0] = fork()) == 0) { // Child
					free(childs);
					// Signals
					signal(SIGINT, SIG_DFL);
					signal(SIGSTOP, SIG_DFL);
					
					// stdin
					if (dummies[0].fin) {
						int fin = open(dummies[0].fin, O_RDONLY);
						
						if (fin == -1) {
							printf("File %s not found.\n", dummies[0].fin);
							eval_state = 0;
							goto jump_send;
						}
						
						// fin -> stdin
						close(0);
						dup(fin);
					}
					
					// stdout
					if (dummies[0].fout) {
						int fout;
						if (dummies[0].fout_m == APPEND)
							fout = open(dummies[0].fout, O_CREAT | O_WRONLY | O_APPEND);
						else
							fout = open(dummies[0].fout, O_CREAT | O_WRONLY);
						
						if (fout == -1) {
							printf("File %s not found.\n", dummies[0].fout);
							eval_state = 0;
							goto jump_send;
						}
						
						// fout -> stdout
						close(1);
						dup(fout);
					}
					
					// exec
					execv(dummies[0].name, dummies[0].argv);
					execvp(dummies[0].name, dummies[0].argv);
					printf("Execution error: File not found.\n");
					// printf("%s\n", explain_execv(dummies[0].name, dummies[0].argv));
					eval_state = 0;
					goto jump_send;
				}
				
				while (wait(NULL) > 0);
				childs_count = 0;
				free(childs);
				childs = NULL;
				
			} else {
				// Dummies structure:
				// [#1: in: file/stdio, out: #2] | ... | [#k: in: #k-1, out: #k+1] | ... | [#N: in: #N-1, out: file/stdout]
				
				for (int i = 1; i < ndummies - 1; ++i)
					if (dummies[i].fin || dummies[i].fout) {
						// Conveyor entry cant output to a file.
						printf("Conveyor entry cant output to a file.\n");
						goto jump_send;
					}
				if (dummies[0].fout) {
					// Conveyor entry cant output to a file.
					printf("Conveyor entry cant output to a file.\n");
					goto jump_send;
				}
				if (dummies[ndummies-1].fin) {
					// Conveyor entry cant input to a file.
					printf("Conveyor entry cant input to a file.\n");
					goto jump_send;
				}
				
				childs = malloc(ndummies * sizeof(int));
				childs_count = ndummies;
				
				int fd1[2] = { -1, -1 }; 
				int fd2[2] = { -1, -1 };
				for (int procind = 0; procind < ndummies; ++procind) {
					fd2[0] = fd1[0];
					fd2[1] = fd1[1];
					
					// 1 -> 2, 2 -> 3, ..., N-1 -> N: total: N-1 pipe
					if (procind < ndummies - 1)
						pipe(fd1);
					
					if ((childs[procind] = fork()) == 0) {
						free(childs); // Release
						// Signals
						signal(SIGINT, SIG_DFL);
						signal(SIGSTOP, SIG_DFL);
						
						if (procind == 0) {
							printf("Executing %s as primary\n", dummies[procind].name);
							close(1);      // stdout -> pipe::write
							dup(fd1[1]);   // close pipe::write
							close(fd1[0]); // disable pipe::read
							close(fd1[1]); // disable pipe::write
						} else if (procind == ndummies - 1) {
							printf("Executing %s as final\n", dummies[procind].name);
							close(0);      // stdin -> pipe::read
							dup(fd2[0]);   // close pipe::read
							close(fd2[0]); // disable pipe::read
							close(fd2[1]); // disable pipe::write
						} else {
							printf("Executing %s as middle\n", dummies[procind].name);
							close(0);      // stdin -> pipe::read
							dup(fd2[0]);   // close pipe::write
							close(fd2[0]); // disable pipe::read
							close(fd2[1]); // disable pipe::write
							close(1);      // stdout -> pipe::write
							dup(fd1[1]);   // close pipe::write
							close(fd1[0]); // disable pipe::read
							close(fd1[1]); // disable pipe::write
						}
					
						// exec
						execv(dummies[procind].name, dummies[procind].argv);
						execvp(dummies[procind].name, dummies[procind].argv);
						printf("Execution error: File not found.\n");
						eval_state = 0;
						goto jump_send;
					}
					
					// Close read/write after fork
					if (fd2[0] != -1) 
						close(fd2[0]);
					if (fd2[1] != -1)
						close(fd2[1]);
				}
				
				// Wait for all childs
				while (wait(NULL) > 0);
				childs_count = 0;
				free(childs);
				childs = NULL;
			}
		}
		
	jump_send:
		for (int i = 0; i < ndummies; ++i)
			free(dummies[i].argv);
		free(dummies);
	}
	
jump_end:
	
	for (int i = 0; comandline[i].type != NONE; ++i)
		free(comandline[i].string);
	
	free(comandline);
};

// Redirect all signals to child processes.
void sig_handle(int sig) {
	signal(SIGINT, &sig_handle);
	signal(SIGSTOP, &sig_handle);
	
	if (!childs_count)
		print_shell_line();
	else
		for (int i = 0; i < childs_count; ++i)
			kill(childs[i], sig);
};

void print_shell_line() {
	char pathbuf[PATHBUF_SIZE];
	getcwd(pathbuf, PATHBUF_SIZE);
	printf("\nshell:%s>", pathbuf);
};

int main(int argc, char **argv) {
	// No arguments.
	signal(SIGINT, &sig_handle);
	signal(SIGSTOP, &sig_handle);
	
	eval_state = 1;
	while (eval_state) {
		print_shell_line();
		eval();
	}
	
	return 0;
};

