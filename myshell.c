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

    char buf[2048];

    //writing end: 1, reading end: 0
    int pipes[2][2];
    bool first_is_echo;

    int i;
    //todo error handling
    pipe(pipes[0]);
    pipe(pipes[1]);
    
    //for (i = N_COMMANDS - 1; i >= 0; i--)
    for(i = 0; i < N_COMMANDS; i++)
    {
        //todo error handling
        pid = fork();

        if(pid == 0)//child
        {
            printf("i am %d entering\n", i);
            fflush(stdout);
            if(i == 0 && i == N_COMMANDS - 1)
            {
                close_entire_pipe(pipes[0]);
                close_entire_pipe(pipes[1]);
            }
            else if(i == 0)
            {
                close_entire_pipe(pipes[0]);
                
                close(pipes[1][READING_END]);
                dup2(pipes[1][WRITING_END], STDOUT_FILENO);
                close(pipes[1][WRITING_END]);
            }
            else if(i < N_COMMANDS - 1)
            {
                //i == 1
                close(pipes[i%2][WRITING_END]);
                dup2(pipes[i%2][READING_END], STDIN_FILENO);
                close(pipes[i%2][READING_END]);
                
                close(pipes[i%2 - 1][READING_END]);
                dup2(pipes[i%2 - 1][WRITING_END], STDOUT_FILENO);
                close(pipes[i%2 - 1][WRITING_END]);
            }
            else 
            {
                close(pipes[i%2][WRITING_END]);
                dup2(pipes[i%2][READING_END], STDIN_FILENO);
                close(pipes[i%2][READING_END]);

                close_entire_pipe(pipes[i%2 - 1]);
            }
            fprintf(stderr, "i am %d, executing\n", i);
            fflush(stderr);
            execvp(line->commands[i].filename, line->commands[i].argv);
            perror("execvp");
            exit(EXIT_FAILURE);
         
        }
    }
    close_entire_pipe(pipes[0]);
    close_entire_pipe(pipes[1]);
    
    for(i = 0; i < N_COMMANDS; i++)
    {
        wait(NULL);
        if (i == 0)
        {

        }

        printf("%d died\n", i);
        fflush(stdout);
    }
    

    //creo q hay q tratar el caso comando i=0 echo, agregarle un EOF
    
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
// if (i == 0 && i != N_COMMANDS - 1)
            // {
            //     close_entire_pipe(pipelef);
            //     close(pipe_RIGHT[0]);
            //     dup2(pipe_RIGHT[1], STDOUT_FILENO);
            //     close(pipe_RIGHT[1]);
            // }
            // else if (i % 2 == 1)
            // {
            //     close(pipe_RIGHT[1]);
            //     dup2(pipe_RIGHT[0], STDIN_FILENO);
            //     close(pipe_RIGHT[0]);
            //     close(pipelef[0]);
            //     dup2(pipelef[1], STDOUT_FILENO);
            //     close(pipelef[1]);
            // }   
            // else if (i % 2 == 0)
            // {
            //     close(pipelef[1]);
            //     dup2(pipelef[0], STDIN_FILENO);
            //     close(pipelef[0]);

            //     close(pipe_RIGHT[0]);
            //     dup2(pipe_RIGHT[1], STDOUT_FILENO);
            //     close(pipe_RIGHT[1]);
            // } 