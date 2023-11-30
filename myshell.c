#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

void close_non_adyacent_pipes(int ** pipes, const int my_i, const int N_COMMANDS)
{
    int j;
    const int N_PIPES = N_COMMANDS - 1;
    for(j = my_i + 1; j < N_PIPES; j++)
    {
        close_entire_pipe(pipes[j]);
    }
    for(j = my_i - 2; j >= 0; j--)
    {
        close_entire_pipe(pipes[j]);
    }
}

void close_entire_pipe(const int pipe[2])
{close(pipe[0]); close(pipe[1]);}

#define BUFFER_SIZE 2048

#define READING_END 0
#define WRITING_END 1

int execute_line(tline * line)
{
    //TODO
    char const *native_commands[] = {};
    pid_t pid;
    const unsigned int N_COMMANDS = line->ncommands;
    int i, j;

    //writing end: 1, reading end: 0
    int **pipes;
    //NO OLVIDARSE DE FREEAR DESP
    pipes = (int **)malloc((N_COMMANDS-1)*sizeof(int**));

    for(i = 0; i < N_COMMANDS - 1; i++)
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
            if(i == 0 && N_COMMANDS > 1)
            {
                close(pipes[i][READING_END]);
                dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                close(pipes[i][WRITING_END]);
                
                close_non_adyacent_pipes(pipes, i, N_COMMANDS);
            }
            else if(i < N_COMMANDS - 1)
            {
                close(pipes[i - 1][WRITING_END]);
                dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                close(pipes[i - 1][READING_END]);

                close(pipes[i][READING_END]);
                dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                close(pipes[i][WRITING_END]);

                close_non_adyacent_pipes(pipes, i, N_COMMANDS);
            }
            else
            {
                close(pipes[i - 1][WRITING_END]);
                dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                close(pipes[i - 1][READING_END]);

                close_non_adyacent_pipes(pipes, i, N_COMMANDS);
            }
            
            fprintf(stderr, "i am %d, executing\n", i);
            fflush(stderr);
            execvp(line->commands[i].filename, line->commands[i].argv);
            perror("execvp");
            exit(EXIT_FAILURE);
         
        }
    }
    for(i = 0; i < N_COMMANDS - 1; i++)
    {
        close_entire_pipe(pipes[i]);
    }
    
    for(i = 0; i < N_COMMANDS; i++)
    {
        wait(NULL);
        printf("%d died\n", i);
        fflush(stdout);
    }
        
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
