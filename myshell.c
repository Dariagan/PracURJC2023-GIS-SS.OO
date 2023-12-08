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
#include <sys/stat.h>
#include <stdbool.h>
#include <pthread.h>
#define true 1
#define false 0
#define BUFFER_SIZE 2048
#define READING_END 0
#define WRITING_END 1

typedef enum{
    RUNNING,
    SUSPENDED,
    FAILED,//Usar status para ver por qué
    DONE //AL MOSTRAR ESTE ESTADO POR PRIMERA VEZ, EL JOB DESPARECE
} JobState;

typedef struct 
{
    unsigned int job_unique_id;
    tline line;
    JobState state;
    pid_t* children_arr;
    unsigned int currently_waited_child_i;
    pthread_t handler_thread_id;//para q cuando se llame la signal, el thread q corresponda haga lo q tenga q hacer
} Job;

static pthread_mutex_t reading_or_modifying_bg_jobs_mtx;

//NO ITERAR SOBRE O MODIFICAR SIN MUTEX ⚠️
static Job* bg_jobs;
//NO MODIFICAR SIN MUTEX ⚠️
static unsigned int bg_jobs_arr_size = 0;
//NO MODIFICAR SIN MUTEX ⚠️
static unsigned int next_job_uid_to_give = 0;

//SOLO LLAMAR DESDE DENTRO DE UN MUTEX ⚠️
void remove_completed_job(const unsigned int job_to_remove_uid)
{
    int previous_i; int added_jobs_count = 0; 
    Job* previous_bg_jobs;
    if(bg_jobs_arr_size == 0) return;

    previous_bg_jobs = bg_jobs;
    bg_jobs_arr_size--;
    if(bg_jobs_arr_size > 0)
    {
        bg_jobs = (Job*)malloc((bg_jobs_arr_size)*sizeof(Job));
        for(previous_i = 0; previous_i < bg_jobs_arr_size + 1; previous_i++)
        {   
            if(previous_bg_jobs[previous_i].job_unique_id != job_to_remove_uid)
            {
                bg_jobs[added_jobs_count++] = previous_bg_jobs[previous_i];
            }
        }
    }
    free(previous_bg_jobs);
}

void change_job_state(const unsigned int job_uid, const JobState new_state)
{
    int i;
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(i = 0; i < bg_jobs_arr_size; i++)
    {   
        if(bg_jobs[i].job_unique_id == job_uid)
        {
            bg_jobs[i].state = new_state;
            break;
        }
    }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
}

void change_job_currently_waited_child_i(const unsigned int job_uid, const unsigned int new_current_i)
{
    int i;
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(i = 0; i < bg_jobs_arr_size; i++)
    {   
        if(bg_jobs[i].job_unique_id == job_uid)
        {
            bg_jobs[i].currently_waited_child_i = new_current_i;
            break;
        }
    }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
}
typedef struct {
    int** used_pipes_arr;
    pid_t* children_pids_arr; 
    tline line;
} t_args_struct;
void* add_and_process_bg_job(void* vargs)
{
    t_args_struct args = *(t_args_struct*)vargs;
    free(vargs);
    unsigned int command_i;
    Job new_job;
    new_job.state = RUNNING;
    new_job.children_arr = args.children_pids_arr;
    new_job.line = args.line;
    new_job.handler_thread_id = pthread_self();
    new_job.currently_waited_child_i = 0;

    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);

    new_job.job_unique_id = next_job_uid_to_give ++;
    bg_jobs_arr_size ++;
    if(bg_jobs_arr_size == 1)
        bg_jobs = (Job*)malloc(sizeof(Job));
    else
        bg_jobs = (Job*)realloc(bg_jobs, bg_jobs_arr_size*sizeof(Job));

    bg_jobs[bg_jobs_arr_size - 1] = new_job;
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);

    for(command_i = 0; command_i < new_job.line.ncommands; command_i++)
    {
        waitpid(new_job.children_arr[command_i], NULL, 0);
        if (command_i < new_job.line.ncommands - 1)
            free((args.used_pipes_arr)[command_i]);

        change_job_currently_waited_child_i(new_job.job_unique_id, command_i);
        
        printf("\nchild i=%d of job w/ uid %d died\n", command_i, new_job.job_unique_id);
        printf("msh> ");
        fflush(stdout);
    }
    free(args.used_pipes_arr);
    change_job_state(new_job.job_unique_id, DONE);

}

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

static const char CD[] = "cd";
static const char JOBS[] = "jobs";
static const char FG[] = "fg";
static const char EXIT[] = "exit";
static const char UMASK[] = "umask";

static const char * const BUILTIN_COMMANDS[] = {CD, JOBS, FG, EXIT, UMASK};
static const __u_char N_BUILTIN_COMMANDS = sizeof(BUILTIN_COMMANDS)/sizeof(char*);

bool is_builtin_command(tcommand* command_data)
{
    const char* const command_name = command_data->argv[0];
    int i;
    for(i = 0; i < N_BUILTIN_COMMANDS; i++)
        if(strcmp(BUILTIN_COMMANDS[i], command_name) == 0)
            return true;
    return false;
}

int execute_cd(tcommand* command_data)
{
    const char* target_directory = command_data->argv[1];

    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", CD);
        return EXIT_FAILURE;
    }
    if (strcmp(target_directory, "~") == 0)
    {
        target_directory = getenv("HOME");
        if (target_directory == NULL) {
            fprintf(stderr, "Failure: HOME environment variable not set.\n");
            return EXIT_FAILURE;
        }
    }
    if (chdir(target_directory) != 0) {
        perror(CD);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int execute_jobs(tcommand* command_data)
{
    int job_i, command_i;
    if (command_data->argc != 1) {
        fprintf(stderr, "Usage: %s \n", JOBS);
        return EXIT_FAILURE;
    }
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(job_i = 0; job_i < bg_jobs_arr_size; job_i++)
    {
        //TODO MOSTRAR EL ESTADO DEL JOB, Y LA LÍNEA EJECUTADA
        Job* job = &(bg_jobs[job_i]);
        printf("[%d][UID:%u] job ", job_i, job->job_unique_id);
        for(command_i = 0; command_i < job->line.ncommands - 1; command_i++)
        {
            printf("%s ") //ver varargs
        }
        printf(" &\n");
        if(job->state == DONE)
        {
            remove_completed_job(job->job_unique_id);
            job_i--;
        }
    }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
    return EXIT_SUCCESS;
}

int execute_fg(tcommand* command_data)
{

}

int execute_exit(tcommand* command_data)
{//hay q parar los jobs q estén en el background. parar el programa seguramente
    
}

int execute_umask(tcommand* command_data)
{
    mode_t new_mask;
    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <octal-mask>\n", UMASK);
        return EXIT_FAILURE;
    }
    if (sscanf(command_data->argv[1], "%o", &new_mask) != 1) {
        fprintf(stderr, "Failure: Invalid octal mask format.\n");
        return EXIT_FAILURE;
    }
    umask(new_mask);

    return EXIT_SUCCESS;
}


int execute_built_in_command(tcommand* command_data)
{
    const char* const command_name = command_data->argv[0];
    if(strcmp(CD, command_name) == 0)
    {
        return execute_cd(command_data);
    }
    else if(strcmp(JOBS, command_name) == 0)
    {
        return execute_jobs(command_data);
    }
    else if(strcmp(FG, command_name) == 0)
    {
        return execute_fg(command_data);
    }
    else if(strcmp(EXIT, command_name) == 0)
    {
        return execute_exit(command_data);
    }
    else if(strcmp(UMASK, command_name) == 0)
    {
        return execute_umask(command_data);
    }
    fprintf(stderr, "INTERNAL ERROR: BUILT-IN COMMAND IS NOT HANDLED BY PROGRAM\n");
    exit(EXIT_FAILURE);
}

int execute_line(tline * line)
{
    pid_t current_pid;
    pid_t* all_pids;
    FILE *file;
    const unsigned int N_COMMANDS = line->ncommands;
    const unsigned int N_PIPES = N_COMMANDS - 1;
    bool builtin_command_present = false;
    bool input_from_file = line->redirect_input != NULL;
    bool output_to_file = line->redirect_output != NULL;
    bool output_stderr_to_file = line->redirect_error != NULL;
    int i, j;
    int **pipes;

    all_pids = (pid_t*)malloc(N_COMMANDS*sizeof(pid_t));

    for(i = 0; i < N_COMMANDS && !builtin_command_present; i++)
    {
        builtin_command_present = is_builtin_command(&(line->commands[i]));
    }
    if(builtin_command_present) 
    {
        if(N_COMMANDS > 1)
        {
            fprintf(stderr, "Error: built-in commands cannot be piped\n");
            return EXIT_FAILURE;
        }
        if(input_from_file)
        {
            fprintf(stderr, "Error: built-in commands cannot take input from a file\n");
            return EXIT_FAILURE;
        }
        if(output_to_file)
        {
            fprintf(stderr, "Error: built-in commands cannot output to a file\n");
            return EXIT_FAILURE;
        }
        return execute_built_in_command(&(line->commands[0]));
    }
    //TODO mandar a background

    pipes = (int **)malloc((N_PIPES)*sizeof(int*));

    for(i = 0; i < N_PIPES; i++)
    {
        pipes[i] = (int*)malloc(2*sizeof(int));
        pipe(pipes[i]);
    }
    
    for(i = 0; i < N_COMMANDS; i++)
    {
        current_pid = fork();

        if(current_pid == 0)//hijo
        {
            fprintf(stderr, "i am %d entering\n", i);
            fflush(stderr);

            if(N_PIPES >= 1)
            {
                if(i == 0)//primer comando
                {
                    close(pipes[i][READING_END]);
                    dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                    close(pipes[i][WRITING_END]);
                    
                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
                else if(i < N_COMMANDS - 1)//comando intermedio
                {
                    close(pipes[i - 1][WRITING_END]);
                    dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                    close(pipes[i - 1][READING_END]);

                    close(pipes[i][READING_END]);
                    dup2(pipes[i][WRITING_END], STDOUT_FILENO);
                    close(pipes[i][WRITING_END]);

                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
                else//último comando
                {
                    close(pipes[i - 1][WRITING_END]);
                    dup2(pipes[i - 1][READING_END], STDIN_FILENO);
                    close(pipes[i - 1][READING_END]);

                    close_non_adjacent_pipes(pipes, i, N_PIPES);
                }
            }
            if (access(line->commands[i].filename, F_OK) != 0) {
               fprintf(stderr, "Failure: Command \"%s\" not found\n", line->commands[i].argv[0]);
               fflush(stderr);
               exit(EXIT_FAILURE);
            }
            if(i == 0 && input_from_file)
            {//si es el primer comando y hay q usar un fichero como stdin
                file = freopen(line->redirect_input, "r", stdin);
                if (file == NULL) {
                    fprintf(stderr, "Failed to open file \"%s\" as input\n", line->redirect_input);
                    exit(EXIT_FAILURE);
                }
            }
            if(i == N_COMMANDS - 1)//si es el último comando
            {
                if(output_to_file)
                {
                    file = freopen(line->redirect_output, "w", stdout);
                    if (file == NULL) {
                        fprintf(stderr, "Failed to create file for outputting stdout\n");
                        exit(EXIT_FAILURE);
                    }
                }
                if(output_stderr_to_file)
                {
                    file = freopen(line->redirect_error, "w", stderr);
                    if (file == NULL) {
                        fprintf(stderr, "Failed to create file for outputting stderr\n");
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
        else if (current_pid == -1)
        {
            fprintf(stderr, "Forking for child command %d failed\n", i+1);
            return EXIT_FAILURE;
        }
        else {all_pids[i] = current_pid;}
    }
    for(i = 0; i < N_PIPES; i++)
    {
        close_entire_pipe(pipes[i]);
    }
    
    if( ! line->background)
    {
        for(i = 0; i < N_COMMANDS; i++)
        {
            waitpid(all_pids[i], NULL, 0);
            if (i < N_PIPES)
                free(pipes[i]);

            printf("%d died\n", i);
            fflush(stdout);
        }
        free(pipes);
    }
    else
    {
        pthread_t temp;
        t_args_struct *args = malloc(sizeof(t_args_struct));
        args->children_pids_arr = all_pids;
        args->line = *line;
        args->used_pipes_arr = pipes;
        pthread_create(&temp, NULL, add_and_process_bg_job, (void*)args);
    }

    return 0;
}


//TODO LA SIGNAL DE CTRL+C (EN VEZ DE CERRAR LA SHELL,CANCELA EL COMANDO EJECUTANDOSE ACTUALMENTE EN EL FOREGROUND)
int main(int argc, char const *argv[])
{
    char buf[2048]; char cwd[2048];
    tline * line;
    pthread_mutex_init(&reading_or_modifying_bg_jobs_mtx, NULL);
    srand(time(NULL));

    if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");return EXIT_FAILURE;}
    
    printf("msh %s> ", cwd);	

    while (fgets(buf, sizeof(buf), stdin)) 
    {
        line = tokenize(buf);
        execute_line(line);    
        if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");return EXIT_FAILURE;}
        printf("msh %s> ", cwd);
    }
    return 0;
}
