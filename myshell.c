#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#define true 1
#define false 0

void close_entire_pipe(const int pipe[2])
{close(pipe[0]); close(pipe[1]);}

void close_non_adjacent_pipes(int ** pipes, const int my_i, const int N_PIPES)
{
    int j;
    for(j = my_i - 2; j >= 0; j--)
    {
        close_entire_pipe(pipes[j]);
    }
    for(j = my_i + 1; j < N_PIPES; j++)
    {
        close_entire_pipe(pipes[j]);
    }
    
}


#define BUFFER_SIZE 2048

#define READING_END 0
#define WRITING_END 1

int execute_line(tline * line)
{
    //TODO
    char const *native_commands[] = {};
    pid_t pid;
    FILE *file;
    const unsigned int N_COMMANDS = line->ncommands;
    const unsigned int N_PIPES = N_COMMANDS - 1;
    bool native_command_present = false;

    bool inputFromFile = line->redirect_input != NULL;
    bool outputToFile = line->redirect_output != NULL;
    bool outputStderrToFile = line->redirect_error != NULL;

    if(native_command_present && 
    (N_COMMANDS > 1 || inputFromFile || outputToFile))
    {
        
    }

    int i, j;

    //writing end: 1, reading end: 0
    int **pipes;

    pipes = (int **)malloc((N_PIPES)*sizeof(int**));

    for(i = 0; i < N_PIPES; i++)
    {
        pipes[i] = (int *)malloc(2*sizeof(int*));
        pipe(pipes[i]);
    }
    
    for(i = 0; i < N_COMMANDS; i++)
    {
        //todo error handling
        pid = fork();

        if(pid == 0)//child
        {
            fprintf(stderr, "i am %d entering\n", i);
            fflush(stderr);

            if(N_PIPES >= 1)
            {
                if(i == 0)//first command
                {
                    close(pipes[i][READING_END]);
                    dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                    close(pipes[i][WRITING_END]);
                    
                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
                else if(i < N_COMMANDS - 1)//intermediate command
                {
                    close(pipes[i - 1][WRITING_END]);
                    dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                    close(pipes[i - 1][READING_END]);

                    close(pipes[i][READING_END]);
                    dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                    close(pipes[i][WRITING_END]);

                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
                else//last command
                {
                    close(pipes[i - 1][WRITING_END]);
                    dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                    close(pipes[i - 1][READING_END]);

                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
            }
            if (access(line->commands[i].filename, F_OK) != 0) {
               fprintf(stderr, "Error: Command not found\n");
               fflush(stderr);
               exit(EXIT_FAILURE);
            }
            if(i == 0 && inputFromFile)//if is the first command...
            {
                file = freopen(line->redirect_input, "r", stdin);
                if (file == NULL) {
                    fprintf(stderr, "Error opening file as stdin\n");
                    exit(EXIT_FAILURE);
                }
            }
            if(i == N_COMMANDS - 1)//if is the last command...
            {
                if(outputToFile)
                {
                    fprintf(stderr, "22r");
                    file = freopen(line->redirect_output, "w", stdout);
                    if (file == NULL) {
                        fprintf(stderr, "Error opening file as stdout\n");
                        exit(EXIT_FAILURE);
                    }
                }
                if(outputStderrToFile)
                {
                    fprintf(stderr, "11 stderr");
                    file = freopen(line->redirect_error, "w", stderr);
                    if (file == NULL) {
                        fprintf(stderr, "Error opening file as stderr\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }

            fprintf(stderr, "i am %d, executing\n", i);
            fflush(stderr);
            execvp(line->commands[i].filename, line->commands[i].argv);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    }
    for(i = 0; i < N_PIPES; i++)
    {
        close_entire_pipe(pipes[i]);
    }
    
    for(i = 0; i < N_COMMANDS; i++)
    {
        wait(NULL);
        if (i < N_PIPES)
            free(pipes[i]);

        printf("%d died\n", i);
        fflush(stdout);
    }
    free(pipes);
        
    return 0;
}


int main(int argc, char const *argv[])
{
    char buf[1024];
    tline * line;
    printf("msh> ");	

    while (fgets(buf, 1024, stdin)) {
        line = tokenize(buf);
        execute_line(line);
        
        printf("msh> ");
    }
    return 0;
}
