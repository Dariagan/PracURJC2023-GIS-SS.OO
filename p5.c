#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>



int main()
{
    
    int i; 
	pid_t pid;
	int pipe_brothers[2];
    int pipe_to_parent[2];

    pipe(pipe_to_parent);
    pipe(pipe_brothers);

    char buff[1024];
	
	for(i = 0; i < 2; i++)
	{
		pid = fork();
        
        if(pid == 0)//if is a forked process
        {

            if (i == 0)//first fork's resulting child
            {
                close(pipe_to_parent[0]);//close read
                dup2(pipe_to_parent[1], STDOUT_FILENO);//change our stdout to output to parent
                close(pipe_to_parent[1]);//close pipe's extra FD

                close(pipe_brothers[1]);//close write to brother fork
                dup2(pipe_brothers[0], STDIN_FILENO);//change our stdin to the pipe's reading end
                close(pipe_brothers[0]);//close pipe's extra FD
                
                execlp("tr", "tr", "d", "Z", (char *)NULL);//now this should have its stdin be the pipe's reading end
                perror("execlp");
                exit(EXIT_FAILURE);
			}
			else if (i == 1)//second fork's resulting child
			{
                close(pipe_to_parent[1]);close(pipe_to_parent[0]);//close both ends since we won't talk to the parent

                close(pipe_brothers[0]);//close read
                dup2(pipe_brothers[1], STDOUT_FILENO);//make ourselves output to the writing end of the pipe to our "brother" fork
                close(pipe_brothers[1]);//close write

                execlp("ls", "ls", "-la", (char *)NULL);
                
                perror("execlp");
                exit(EXIT_FAILURE);
		}
			}
	}
    close(pipe_brothers[1]);close(pipe_brothers[0]);
    close(pipe_to_parent[1]);
    read(pipe_to_parent[0], buff, 1024);
    puts(buff);
   
	for(i = 0; i < 2; i++)
	{
		wait(NULL);
	}
    close(pipe_to_parent[0]);
	return 0;
}




