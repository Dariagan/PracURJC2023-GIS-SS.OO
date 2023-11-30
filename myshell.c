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

int execute_line(tline * line)
{
    char const *native_commands[] = {};
    pid_t pid;
    int pipe1[2];
    int i;
    //todo error handling
    pipe(pipe1);

    for (i = 0; i < line->ncommands; i++)
    {
        pid = fork();

        if(pid == 0)//child
        {
            
            execvp(line->commands[i].filename, line->commands[i].argv);
        }
        else//parent
        {
            
        }
                
    }
    
}


int main(int argc, char const *argv[])
{
    

    
	int pipe_brothers[2];
    int pipe_to_parent[2];

    pipe(pipe_to_parent);
    pipe(pipe_brothers);

    char buf[1024];
    tline * line;
    printf("msh> ");	

    while (fgets(buf, 1024, stdin)) {
        line = tokenize(buf);

        
    }
    return 0;
}