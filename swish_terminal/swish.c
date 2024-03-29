#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define CMD_LEN 512
#define PROMPT "@> "

int main(int argc, char **argv) {
    // Task 4: Set up shell to ignore SIGTTIN, SIGTTOU when put in background
    // You should adapt this code for use in run_command().
    struct sigaction sac;
    sac.sa_handler = SIG_IGN;
    if (sigfillset(&sac.sa_mask) == -1) {
        perror("sigfillset");
        return 1;
    }
    sac.sa_flags = 0;
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    strvec_t tokens;
    strvec_init(&tokens);
    job_list_t jobs;
    job_list_init(&jobs);
    char cmd[CMD_LEN];

    printf("%s", PROMPT);
    while (fgets(cmd, CMD_LEN, stdin) != NULL) {
        // Need to remove trailing '\n' from cmd. There are fancier ways.
        int i = 0;
        while (cmd[i] != '\n') {
            i++;
        }
        cmd[i] = '\0';

        if (tokenize(cmd, &tokens) != 0) {
            printf("Failed to parse command\n");
            strvec_clear(&tokens);
            job_list_free(&jobs);
            return 1;
        }
        if (tokens.length == 0) {
            printf("%s", PROMPT);
            continue;
        }
        const char *first_token = strvec_get(&tokens, 0);
        const char *last_token = strvec_get(&tokens, tokens.length-1);

        if (strcmp(first_token, "pwd") == 0) {
            char *buf = malloc(CMD_LEN);
            if(buf == NULL){
                perror("malloc");
            }
            char *ret = getcwd(buf, CMD_LEN);
            if(ret == NULL){
                perror("getcwd");
            } else {
                printf("%s\n", ret);
            }
            free(buf);
        }

        else if (strcmp(first_token, "cd") == 0) {
            char *path = NULL;
            if(tokens.length > 1){
                if((path = strvec_get(&tokens, 1)) == NULL){
                    printf("unable to get argument\n");
                }
            } else {
                if((path = getenv("HOME")) == NULL){
                    printf("unable to get HOME environment\n");
                }
            }
            if(chdir(path) == -1){
                perror("chdir");
            }
        }

        else if (strcmp(first_token, "exit") == 0) {
            strvec_clear(&tokens);
            job_list_free(&jobs); // need to free job list
            break;
        }

        // Task 5: Print out current list of pending jobs
        else if (strcmp(first_token, "jobs") == 0) {
            int i = 0;
            job_t *current = jobs.head;
            while (current != NULL) {
                char *status_desc;
                if (current->status == JOB_BACKGROUND) {
                    status_desc = "background";
                } else {
                    status_desc = "stopped";
                }
                printf("%d: %s (%s)\n", i, current->name, status_desc);
                i++;
                current = current->next;
            }
        }

        // Task 5: Move stopped job into foreground
        else if (strcmp(first_token, "fg") == 0) {
            if (resume_job(&tokens, &jobs, 1) == -1) {
                printf("Failed to resume job in foreground\n");
            }
        }

        // Task 6: Move stopped job into background
        else if (strcmp(first_token, "bg") == 0) {
            if (resume_job(&tokens, &jobs, 0) == -1) {
                printf("Failed to resume job in background\n");
            }
        }

        // Task 6: Wait for a specific job identified by its index in job list
        else if (strcmp(first_token, "wait-for") == 0) {
            if (await_background_job(&tokens, &jobs) == -1) {
                printf("Failed to wait for background job\n");
            }
        }

        // Task 6: Wait for all background jobs
        else if (strcmp(first_token, "wait-all") == 0) {
            if (await_all_background_jobs(&jobs) == -1) {
                printf("Failed to wait for all background jobs\n");
            }
        }

        else {
            int background = 0;
            if(strcmp(last_token, "&") == 0){ // if last token is &, bg process
                background = 1;
                strvec_take(&tokens, tokens.length-1);
            }

            int status;
            pid_t child_pid = fork();
            if(child_pid == -1){ // error
                perror("fork");
            } else if(child_pid == 0){ // child
                run_command(&tokens);
                return 1;
            } else{ // parent
                if(background){ // just add to job list as bg job
                    if(job_list_add(&jobs, child_pid, first_token, JOB_BACKGROUND) == -1){
                        printf("job_list_add\n");
                    }
                }
                else {
                    if(tcsetpgrp(STDIN_FILENO, child_pid) == -1){ // put child in the foreground
                    perror("tcsetpgrp");
                    }
                    if(waitpid(child_pid, &status, WUNTRACED) == -1){ // wait for child
                        perror("wait");
                    }
                    if(tcsetpgrp(STDIN_FILENO, getpid()) == -1){ // restore shell to fg
                        perror("tcsetpgrp");
                    }
                    if(WIFSTOPPED(status)){ // if stopped add to jobs list as a stopped job
                        if(job_list_add(&jobs, child_pid, first_token, JOB_STOPPED) == -1){
                            printf("job_list_add\n");
                        }
                    }
                }
            }
        }

        strvec_clear(&tokens);
        printf("%s", PROMPT);
    }

    return 0;
}
