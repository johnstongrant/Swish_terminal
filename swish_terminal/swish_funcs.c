#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define MAX_ARGS 10

int tokenize(char *s, strvec_t *tokens) {
    char *tok = strtok(s, " "); // get first element

    if(tok == NULL){ // check if string is empty
        printf("string provided contains no elements to tokenize\n");
        return -1;
    }

    while(tok != NULL){ // loop to get rest of elements
        if(strvec_add(tokens, tok) == -1){ // add to tokens list
            printf("Error adding to string vector.\n");
            return -1;
        }
        tok = strtok(NULL, " "); // repeat calls take NULL as first arg
    }
    return 0;
}

int run_command(strvec_t *tokens) {
    int arg_index = tokens->length; // index of the last argument to execvp, will not include redirection operations
    int current = -1; // current index
    // if user wants to redirect input
    if((current = strvec_find(tokens, "<")) >= 0){
        char *input_file = strvec_get(tokens, current+1);
        if(input_file == NULL){
            printf("unable to get input file name\n");
            return 1;
        }
        int in_fd = open(input_file, O_RDONLY);
        if(in_fd == -1){
            perror("Failed to open input file");
            return 1;
        }
        if (dup2(in_fd, STDIN_FILENO) == -1){ // move input from stdin to file
            perror("dup2");
            close(in_fd);
            return 1;
        }
        arg_index = current;
        close(in_fd);
    }
    // redirect output (writing)
    if((current = strvec_find(tokens, ">")) >= 0){
        char *output_file = strvec_get(tokens, current+1);
        if(output_file == NULL){
            printf("unable to get output file name");
            return 1;
        }
        int out_fd = open(output_file, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR);
        if(out_fd == -1){
            perror("Failed to open output file");
            return 1;
        }
        if(dup2(out_fd, STDOUT_FILENO) == -1){ // move output from stdout to file
            perror("dup2");
            close(out_fd);
            return 1;
        }
        if(current < arg_index){
            arg_index = current;
        }
        close(out_fd);
    // redirect output (appending)
    } else if((current = strvec_find(tokens, ">>")) >= 0){
        char *output_file = strvec_get(tokens, current+1);
        if(output_file == NULL){
            printf("unable to get output file name");
            return 1;
        }
        int out_fd = open(output_file, O_CREAT|O_APPEND|O_WRONLY, S_IRUSR|S_IWUSR);
        if(out_fd == -1){
            perror("Failed to open output file");
            return 1;
        }
        if(dup2(out_fd, STDOUT_FILENO) == -1){ // move output from stdout to file
            perror("dup2");
            close(out_fd);
            return 1;
        }
        if(current < arg_index){
            arg_index = current;
        }
        close(out_fd);
    }

    struct sigaction sac; // adapted from code in swish.c
    sac.sa_handler = SIG_DFL; // changed to SIG_DFL
    if (sigfillset(&sac.sa_mask) == -1) {
        perror("sigfillset");
        return 1;
    }
    sac.sa_flags = 0;
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    pid_t child_pid = getpid(); 
    if(setpgid(child_pid, child_pid) == -1){ // change process group id so it's in its own group
        perror("setpgid");
        return 1;
    }

    char *args[MAX_ARGS];
    int i;
    for(i=0; i<arg_index; i++){ // iterates to arg_index, disregarding redirection
        if((args[i] = strvec_get(tokens, i)) == NULL){
            printf("error getting token from strvec\n");
            return 1;
        }
    }
    args[i] = NULL; // set null sentinel
    if(execvp(tokens->data[0], args) == -1){
        perror("exec");
    }

    return 0;
}

int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
    int index = atoi(tokens->data[1]);
    job_t *job = job_list_get(jobs, index); // get current job
    if(job == NULL){
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }
    pid_t pid = job->pid;
    
    if(is_foreground){ // if in the foreground
        if(tcsetpgrp(STDIN_FILENO, pid) == -1){ // set group
            perror("tcsetpgrp");
            return -1;
        }
        if(kill(pid, SIGCONT) == -1){ // send continue signal
            perror("kill");
            return -1;
        }
        int status = -1;
        if(waitpid(pid, &status, WUNTRACED) == -1){ // wait for process
            perror("wait");
            return -1;
        }
        if(WIFEXITED(status) || WIFSIGNALED(status)){ // if exited or signaled, remove from job list
            if(job_list_remove(jobs, index) == -1){
                printf("failed to remove job ffrom job list\n");
                return -1;
            }
        }
        if(tcsetpgrp(STDIN_FILENO, getpid()) == -1){ 
            perror("tcsetpgrp");
            return -1;
        }
    } else { // if in background, set job status accordingly
        job->status = JOB_BACKGROUND;
        if(kill(pid, SIGCONT) == -1){ // send signal to continue
            perror("kill");
            return -1;
        }
    }

    return 0;
}

int await_background_job(strvec_t *tokens, job_list_t *jobs) {
    int index = atoi(tokens->data[1]);
    job_t *job = job_list_get(jobs, index);
    if(job == NULL){
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }
    if(job->status == JOB_BACKGROUND){ // if in the bg
        int status;
        if(waitpid(job->pid, &status, WUNTRACED) == -1){ // wait
            perror("waitpid");
            return -1;
        }
        if(WIFEXITED(status)){ // if exited
            if(job_list_remove(jobs, index) == -1){ // remove from jobs list
                printf("job_list_remove\n");
                return -1;
            }
        }
    } else {
        fprintf(stderr, "Job index is for stopped process not background process\n");
        return -1;
    }

    return 0;
}

int await_all_background_jobs(job_list_t *jobs) {

    job_t *ptr = jobs->head;
    while(ptr != NULL){
        int status;
        if(ptr->status == JOB_BACKGROUND){
            if(waitpid(ptr->pid, &status, WUNTRACED) == -1){
                perror("waitpid");
                return -1;
            }
            if(WIFSTOPPED(status)){
                ptr->status = JOB_STOPPED;
            }
        }
        ptr = ptr->next;
    }

    job_list_remove_by_status(jobs, JOB_BACKGROUND);

    return 0;
}
