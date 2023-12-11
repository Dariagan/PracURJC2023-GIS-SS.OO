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
const char* const stringify_job_state(JobState jobState)
{switch(jobState){
case RUNNING: return "Running"; break;
case SUSPENDED: return "Suspended"; break;
case FAILED: return "Failed"; break;
case DONE: return "Done"; break;
default:fprintf(stderr, "INTERNAL ERROR (at stringify_job_state())\n");exit(EXIT_FAILURE);
}}
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
void deep_free_line_from_job(Job* job);
//SOLO LLAMAR DESDE DENTRO DEL MUTEX⚠️
void remove_completed_job(const unsigned int job_to_remove_uid)
{
    int prev_arr_i; int added_jobs_count = 0; 
    Job* previous_bg_jobs;
    if(bg_jobs_arr_size == 0) {fprintf(stderr, "INTERNAL ERROR: job list is empty at this point\n"); exit(EXIT_FAILURE);}

    previous_bg_jobs = bg_jobs;
    if(-- bg_jobs_arr_size > 0)
    {
        bg_jobs = (Job*)malloc((bg_jobs_arr_size)*sizeof(Job));
        for(prev_arr_i = 0; prev_arr_i < bg_jobs_arr_size + 1; prev_arr_i++)
        {   
            if(previous_bg_jobs[prev_arr_i].job_unique_id != job_to_remove_uid)
            {
                bg_jobs[added_jobs_count++] = previous_bg_jobs[prev_arr_i];
            }
            else deep_free_line_from_job(previous_bg_jobs + prev_arr_i);
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

void deep_copy_line(tline* dest_line, tline* src_line)
{
    int cmd_i, arg_j; tcommand* dest_current_command, *src_current_command;
    *dest_line = *src_line;
    dest_line->commands = malloc(src_line->ncommands*sizeof(tcommand));

    for(cmd_i = 0; cmd_i < src_line->ncommands; cmd_i++)
    {
        dest_current_command = dest_line->commands + cmd_i;
        src_current_command = src_line->commands + cmd_i;

        dest_current_command->filename = strdup(src_current_command->filename);
        dest_current_command->argc = src_current_command->argc;
        dest_current_command->argv = malloc(src_current_command->argc*sizeof(char*));
        for(arg_j = 0; arg_j < src_current_command->argc; arg_j++)
        {
            dest_current_command->argv[arg_j] = strdup(src_current_command->argv[arg_j]);
        }
    }
    if(src_line->redirect_input)
        dest_line->redirect_input = strdup(src_line->redirect_input);
    if(src_line->redirect_output)
        dest_line->redirect_output = strdup(src_line->redirect_output);
    if(src_line->redirect_error)
        dest_line->redirect_error = strdup(src_line->redirect_error);
}
void deep_free_line_from_job(Job* job)
{
    int cmd_i, arg_j; 
    tline* line = &(job->line);
    tcommand* current_command;
    if(line->redirect_input)
        free(line->redirect_input);
    if(line->redirect_output)
        free(line->redirect_output);
    if(line->redirect_error)
        free(line->redirect_error);
        
    for(cmd_i = 0; cmd_i < line->ncommands; cmd_i++)
    {
        current_command = line->commands + cmd_i;
        free(current_command->filename);
        for(arg_j = 0; arg_j < current_command->argc; arg_j++)
        {
            free(current_command->argv[arg_j]);
        }
        free(current_command->argv);
    }
    free(line->commands);
}

//NO MODIFICAR SIN MUTEX ⚠️
static unsigned int next_job_uid_to_assign = 1;

static pthread_t main_thread;
static pid_t* fg_forks_pids_arr;
static unsigned int fg_n_commands = 0;
static bool sent_to_background = false;
static unsigned int fg_waited_i = 0;

static bool fg_execution_cancelled = false;

typedef struct {
    int** used_pipes_arr;
    pid_t* children_pids_arr; 
    tline line;
} AddJobArgs;
void* async_add_and_process_bg_job(void* uncasted_args)
{
    AddJobArgs args = *((AddJobArgs*)uncasted_args);free(uncasted_args);
    unsigned int command_i;
    Job new_job;
    new_job.state = RUNNING;
    new_job.children_arr = args.children_pids_arr;

    new_job.line = args.line;

    new_job.handler_thread_id = pthread_self();
    new_job.currently_waited_child_i = 0;
    
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);

    new_job.job_unique_id = next_job_uid_to_assign ++;

    if(++ bg_jobs_arr_size == 1)
        bg_jobs = malloc(sizeof(Job));
    else
        bg_jobs = realloc(bg_jobs, bg_jobs_arr_size*sizeof(Job));

    bg_jobs[bg_jobs_arr_size - 1] = new_job;
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);

    for(command_i = 0; command_i < new_job.line.ncommands; command_i++)
    {
        if( ! (pthread_self() == main_thread && fg_execution_cancelled))
        {
            waitpid(new_job.children_arr[command_i], NULL, 0);
            change_job_currently_waited_child_i(new_job.job_unique_id, command_i);
        }
        else
        {
            waitpid(new_job.children_arr[command_i], NULL, WNOHANG);
        }

        if (command_i < new_job.line.ncommands - 1)
            free((args.used_pipes_arr)[command_i]);
        
        //fprintf(stdout, "\nchild i=%d of job w/ uid %d died\n", command_i, new_job.job_unique_id);printf("msh> ");fflush(stdout);
    }
    if( ! (pthread_self() == main_thread && fg_execution_cancelled))
        change_job_state(new_job.job_unique_id, DONE);
    
    free(args.used_pipes_arr);
    free(args.children_pids_arr);
    
}

void close_entire_pipe(const int pipe[2])
{close(pipe[0]); close(pipe[1]);}

void close_non_adjacent_pipes(int ** pipes, const int my_i, const int N_PIPES)
{int j;for(j = my_i - 2; j >= 0; j--) close_entire_pipe(pipes[j]);
for(j = my_i + 1; j < N_PIPES; j++) close_entire_pipe(pipes[j]);}

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
    int job_i, cmd_i, arg_j, jobs_to_remove_count = 0; 
    unsigned int* completed_job_uids;
    Job* job;
    tcommand* command;
    if (command_data->argc != 1) {
        fprintf(stderr, "Usage: %s \n", JOBS);
        return EXIT_FAILURE;
    }
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(job_i = 0; job_i < bg_jobs_arr_size; job_i++)
    {
        job = bg_jobs + job_i;
        printf("[%dº][UID:%u] job ", job_i, job->job_unique_id);
        for(cmd_i = 0; cmd_i < job->line.ncommands; cmd_i++)
        {
            command = job->line.commands + cmd_i;
            for(arg_j = 0; arg_j < command->argc; arg_j++)
            {
                printf("%s ", command->argv[arg_j]);
            }
            if(cmd_i <  job->line.ncommands - 1)
            {
                printf("| ");
            }
        }
        if(job->line.redirect_input != NULL)
        {
            printf(" < %s", job->line.redirect_input);
        }
        if(job->line.redirect_output != NULL)
        {
            printf(" > %s", job->line.redirect_output);
        }
        if(job->line.redirect_error != NULL)
        {
            printf(" >& %s", job->line.redirect_error);
        }

        printf(" STATUS: %s\n", stringify_job_state(job->state));
        if(job->state == DONE)
        {
            if(jobs_to_remove_count ++ == 0)
            {
                completed_job_uids = malloc(sizeof(unsigned int));
                completed_job_uids[0] = job->job_unique_id;
            }
            else
            {
                completed_job_uids = realloc(completed_job_uids, jobs_to_remove_count*sizeof(unsigned int));
                completed_job_uids[jobs_to_remove_count-1] = job->job_unique_id;
            }
        }
    }
    for(job_i = 0; job_i < jobs_to_remove_count; job_i++)
    {
        remove_completed_job(completed_job_uids[job_i]);
    }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
    
    if(jobs_to_remove_count > 0)
    {
        free(completed_job_uids);
    }
    
    return EXIT_SUCCESS;
}//TODO: LLENAR TODO DE COMENTARIOS
//llamar solo desde dentro de mutex
Job* find_bg_job(unsigned int uid){
    unsigned int i;
    for(i = 0; i < bg_jobs_arr_size; i++)
    {
        if (bg_jobs[i].job_unique_id == uid){
            
            return bg_jobs + i;
        }
    }
    return NULL;
}
int execute_fg(tcommand* command_data)
{
    int uid; Job* job;
    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <job UID>\n", FG);
        return EXIT_FAILURE;
    }
    uid = atoi(command_data->argv[1]);
    if(uid)
    {   
        pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
        job = find_bg_job(uid);
        if(job)
        {
            if(job->state != DONE)
            {
                fg_waited_i = job->currently_waited_child_i;
                fg_forks_pids_arr = job->children_arr;
                fg_n_commands = job->line.ncommands;
                main_thread = job->handler_thread_id;
                remove_completed_job(uid);
                pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
                pthread_join(main_thread, NULL);
                main_thread = pthread_self();
                return EXIT_SUCCESS;
            }
            else fprintf(stderr, "Job with UID=%d has already finished\n", uid);
        }
        else fprintf(stderr, "Job with UID=%d not found\n", uid);
        
        pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
        return EXIT_FAILURE;
    }
    else
    {
        fprintf(stderr, "Specified Job UID must be a strictly positive integer\n");
        return EXIT_FAILURE;
    }
}
//USAR DENTRO DE MUTEX
void broadcast_signal(int signal)
{
    int jobs_i = 0, j = 0; Job* job;
    for(jobs_i = 0; jobs_i < bg_jobs_arr_size; jobs_i++)
    {   
        job = bg_jobs + jobs_i;
        for(j = 0; j < job->line.ncommands; j++)
            kill(job->children_arr[j], signal);
    }
}

int execute_exit(tcommand* command_data)
{
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    broadcast_signal(SIGTERM);
    sleep(1);
    broadcast_signal(SIGKILL);
    exit(0);
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

typedef struct {
    pid_t* forks_pids_arr;
    int waited_i;
    int n_commands;
} AsyncKillArgs;
void* async_delayed_force_kill(void * uncasted_args)
{
    int i;
    AsyncKillArgs args = *((AsyncKillArgs*)uncasted_args);free(uncasted_args);
    sleep(10);
    for(i = args.waited_i; i < args.n_commands; i++)
        kill(args.forks_pids_arr[i], SIGKILL);
}
void stop_foreground_execution(int signal)
{
    unsigned int i;
    AsyncKillArgs* args;
    pthread_t placeholder;
    if(pthread_self() == main_thread && signal == SIGINT && !sent_to_background && fg_n_commands)
    {
        for(i = fg_waited_i; i < fg_n_commands; i++)
        {
            kill(fg_forks_pids_arr[i], SIGTERM);
        }
        args = (AsyncKillArgs*)malloc(sizeof(AsyncKillArgs));
        args->n_commands = fg_n_commands;
        args->forks_pids_arr = fg_forks_pids_arr;
        args->waited_i = fg_waited_i;
        
        pthread_create(&placeholder, NULL, async_delayed_force_kill, (void*)args);
        
        fg_execution_cancelled = true;
    }
}

int run_line(tline* line)
{
    pid_t current_pid;
    const unsigned int N_PIPES = line->ncommands - 1;
    int** pipes_arr;
    bool builtin_command_present = false;
    bool input_from_file = line->redirect_input != NULL;
    bool output_to_file = line->redirect_output != NULL;
    bool output_stderr_to_file = line->redirect_error != NULL;
    FILE *file;
    int i; pthread_t placeholder;
    
    fg_n_commands = N_PIPES + 1;
    sent_to_background = line->background;
    fg_forks_pids_arr = (pid_t*)malloc(fg_n_commands*sizeof(pid_t));

    if(!fg_n_commands) return 0;

    for(i = 0; i < fg_n_commands && !builtin_command_present; i++)
    {
        builtin_command_present = is_builtin_command(line->commands + i);
    }
    if(builtin_command_present) 
    {
        if(fg_n_commands > 1)
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
        if(line->background)
        {
            fprintf(stderr, "Error: built-in commands cannot be executed in the background\n");
            return EXIT_FAILURE;
        }
        return execute_built_in_command(&(line->commands[0]));
    }
    pipes_arr = (int **)malloc((N_PIPES)*sizeof(int*));

    for(i = 0; i < N_PIPES; i++)
    {
        pipes_arr[i] = (int*)malloc(2*sizeof(int));
        pipe(pipes_arr[i]);
    }
    
    for(i = 0; i < fg_n_commands; i++)
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
                    close(pipes_arr[i][READING_END]);
                    dup2(pipes_arr[i][WRITING_END], STDOUT_FILENO);
                    close(pipes_arr[i][WRITING_END]);
                    
                    close_non_adjacent_pipes(pipes_arr, i, N_PIPES);
                }
                else if(i < fg_n_commands - 1)//comando intermedio
                {
                    close(pipes_arr[i - 1][WRITING_END]);
                    dup2(pipes_arr[i - 1][READING_END], STDIN_FILENO);
                    close(pipes_arr[i - 1][READING_END]);

                    close(pipes_arr[i][READING_END]);
                    dup2(pipes_arr[i][WRITING_END], STDOUT_FILENO);
                    close(pipes_arr[i][WRITING_END]);

                    close_non_adjacent_pipes(pipes_arr, i, N_PIPES);
                }
                else//último comando
                {
                    close(pipes_arr[i - 1][WRITING_END]);
                    dup2(pipes_arr[i - 1][READING_END], STDIN_FILENO);
                    close(pipes_arr[i - 1][READING_END]);

                    close_non_adjacent_pipes(pipes_arr, i, N_PIPES);
                }
            }
            if (access(line->commands[i].filename, F_OK) != 0) {
               fprintf(stderr, "Failure: Command \"%s\" not found\n", line->commands[i].argv[0]);
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
            if(i == fg_n_commands - 1)//si es el último comando...
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
            //fprintf(stdout, "i am %d, executing\n msh>", i);fflush(stdout);

            if(i > 0)//esperar q el hermano anterior esté muerto (cuando la señal devuelve 1)
                while(kill(fg_forks_pids_arr[i-1], 0) != -1)
                    usleep(10);
            
            execvp(line->commands[i].filename, line->commands[i].argv);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
        else if (current_pid == -1)
        {
            fprintf(stderr, "Forking for child command %d failed\n", i+1);
            return EXIT_FAILURE;
        }
        else {fg_forks_pids_arr[i] = current_pid;}
    }
    for(i = 0; i < N_PIPES; i++)
    {
        close_entire_pipe(pipes_arr[i]);
    }
    
    if( ! line->background)
    {
        for(fg_waited_i = 0; fg_waited_i < fg_n_commands; fg_waited_i++)
        {
            if(!fg_execution_cancelled)
                waitpid(fg_forks_pids_arr[fg_waited_i], NULL, 0);
            else//TODO USAR EL STATUS DEVUELTO PA ALGO
                waitpid(fg_forks_pids_arr[fg_waited_i], NULL, WNOHANG);//creo q hay q usar el return para algo
                
            if (fg_waited_i < N_PIPES)
                free(pipes_arr[fg_waited_i]);

            printf("%d died\n", fg_waited_i);
            fflush(stdout);
        }
        free(pipes_arr);
        free(fg_forks_pids_arr);
    }
    else
    {
        AddJobArgs *args = malloc(sizeof(AddJobArgs));
        args->children_pids_arr = fg_forks_pids_arr;
        deep_copy_line(&args->line, line);
        args->used_pipes_arr = pipes_arr;
        pthread_create(&placeholder, NULL, async_add_and_process_bg_job, (void*)args);
    }

    return 0;
}

int do_await_input_loop()
{
    char buf[BUFFER_SIZE]; char cwd[BUFFER_SIZE];
    tline * line;
    if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");exit(EXIT_FAILURE);} 
    printf("msh %s> ", cwd);	
    while (fgets(buf, sizeof(buf), stdin)) 
    {
        main_thread = pthread_self();
        line = tokenize(buf);
        run_line(line);    
        fg_execution_cancelled = false;
        if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");exit(EXIT_FAILURE);}
        printf("msh %s> ", cwd);
    }
    exit(EXIT_SUCCESS);
}

int main(int argc, char const *argv[])
{
    pthread_mutex_init(&reading_or_modifying_bg_jobs_mtx, NULL);
    signal(SIGINT, stop_foreground_execution);
    do_await_input_loop();
}
//TODO: LLENAR TODO DE COMENTARIOS