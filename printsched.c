#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <time.h>
#include <pthread.h>

#define NUM_JOBS 1000
#define NUM_PRINTERS 10

typedef enum {
    WAITING,
    PRINTING,
    DONE
} job_state_t;

typedef enum {
    FCFS,
    SJF,
    BALANCED
} algo_sched;

struct job_t{
    int id;
    job_state_t state;
    char* filename;
    time_t submission_time;
    time_t start_time;
    time_t end_time;
    long duration;
    int priority;
    pid_t pid;
    struct job_t* prev;
    struct job_t* next;
};

typedef struct {
    struct job_t* head;
    struct job_t* dequeued_jobs;
    algo_sched algo;
    pthread_mutex_t lock;
    pthread_cond_t queue_cond;
    pthread_cond_t drain_cond;
    int hurry_count;
} job_queue_t;

typedef struct {
    job_queue_t* job_queue;
} printer_t;

int job_id = 1;
int quit_flag = 0;
job_queue_t* job_queue = NULL;
pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;

int is_number(char number[]) {
    int i = 0;
    //checking for negative numbers
    if (number[0] == '-')
        i = 1;
    for (; number[i] != 0; i++) {
        //if (number[i] > '9' || number[i] < '0')
        if (!isdigit(number[i]))
            return 0;
    }
    return 1;
}

void init_job_queue() {
    job_queue = (job_queue_t*) malloc(sizeof(job_queue_t));
    job_queue->head = NULL;
    job_queue->dequeued_jobs = NULL;
    job_queue->algo = FCFS;
    job_queue->hurry_count = 0;
    pthread_mutex_init(&job_queue->lock, NULL);
    pthread_cond_init(&job_queue->queue_cond, NULL);
    pthread_cond_init(&job_queue->drain_cond, NULL);
}

void enqueue(job_queue_t *queue, struct job_t *job) {
    pthread_mutex_lock(&queue->lock);
    job->next = NULL;
    job->prev = NULL;
    if (queue->head == NULL) 
        queue->head = job;
    else {
        struct job_t *curr = queue->head;
        while (curr->next != NULL)
            curr = curr->next;
        curr->next = job;
        job->prev = curr;
    }
    pthread_cond_signal(&queue->queue_cond);
    pthread_mutex_unlock(&queue->lock);
}


struct job_t* dequeue(job_queue_t *queue) {
    pthread_mutex_lock(&queue->lock);

    struct job_t *selected_job = NULL;
    struct job_t *prev_job = NULL;

    while (selected_job == NULL && !quit_flag) {
        struct job_t* curr = queue->head;
        struct job_t* prev = NULL;

        if (queue->hurry_count > 0) { // check if there are hurried jobs
            struct job_t* curr = queue->head;
            struct job_t* prev = NULL;
            while (curr != NULL) {
                if (curr->state != DONE && curr->priority == 1) {
                    if (selected_job == NULL || curr->id < selected_job->id) {
                        selected_job = curr;
                        prev_job = prev;
                    }
                }
                prev = curr;
                curr = curr->next;
            }
        } 
        else {
            if (queue->algo == FCFS) {
                selected_job = queue->head;
                prev_job = NULL;
                while (selected_job != NULL && selected_job->state == DONE) {
                    prev_job = selected_job;
                    selected_job = selected_job->next;
                }
            } 
            else {
                curr = queue->head;
                prev = NULL;

                while (curr != NULL) {
                    if (curr->state != DONE && (selected_job == NULL ||
                        (queue->algo == SJF && curr->duration < selected_job->duration) ||
                        /* Balanced Algorithm:
                        This will prioritize shorter jobs that have been waiting longer
                        and longer jobs that have been waiting for a shorter time
                        In other words, the job with a shorter duration and an earlier submission time will be prioritized
                        If a job has a longer duration, it will be prioritized only if its submission time is more recent than the job
                        with shorter duration.
                        By doing this, I ensure that shorter jobs that have been waiting longer get executed first, while longer jobs with
                        more recent submission times will not be completely starved
                        */
                        (queue->algo == BALANCED && ((curr->duration < selected_job->duration && curr->submission_time < selected_job->submission_time) || 
                            (curr->duration > selected_job->duration && curr->submission_time > selected_job->submission_time))))) {
                            selected_job = curr;
                            prev_job = prev;
                    }
                    prev = curr;
                    curr = curr->next;
                }
            }
        }

        if (selected_job == NULL) 
            pthread_cond_wait(&queue->queue_cond, &queue->lock);
    }

    if (selected_job != NULL) {
        if (prev_job == NULL) {
            queue->head = selected_job->next;
        } else {
            prev_job->next = selected_job->next;
        }
        if (selected_job->next != NULL) {
            selected_job->next->prev = selected_job->prev;
        }

        // Add selected_job to the dequeued_jobs list
        if (queue->dequeued_jobs == NULL) {
            queue->dequeued_jobs = selected_job;
            selected_job->next = NULL;
        } else {
            struct job_t *dequeued_iter = queue->dequeued_jobs;
            while (dequeued_iter->next != NULL) {
                dequeued_iter = dequeued_iter->next;
            }
            dequeued_iter->next = selected_job;
            selected_job->next = NULL;
        }
    }

    // decrement the hurry_count if the dequeued job had priority 1
    if (selected_job != NULL && selected_job->priority == 1) 
        queue->hurry_count--;
    
    pthread_mutex_unlock(&queue->lock);
    return selected_job;
}

void run_printer_simulator(struct job_t* job) {
    job->start_time = time(NULL);
    job->state = PRINTING;
    pid_t pid = fork();
    if (pid == 0) {
        // child process
        if (execl("./printersim", "printersim", job->filename, (char *)NULL) < 0) {
            perror("execl"); // if execl returns, an error occurred
            exit(1);
        }
    } 
    else if (pid > 0) {
        // parent process
        job->pid = pid;
        int status;
        waitpid(pid, &status, 0);
        job->state = DONE;
        job->end_time = time(NULL);
    } 
    else 
        perror("fork");
}

void* printer_thread(void* arg) {
    printer_t *printer = (printer_t*)arg;

    while (!quit_flag) {
        struct job_t *job = dequeue(printer->job_queue);
        if (job != NULL) {
            run_printer_simulator(job);

            // signal for drainning process
            pthread_mutex_lock(&printer->job_queue->lock);
            if (job->next == NULL)
                pthread_cond_signal(&printer->job_queue->drain_cond);
            pthread_mutex_unlock(&printer->job_queue->lock);

            // signal for wait_job process
            pthread_mutex_lock(&job_lock);
            pthread_cond_broadcast(&job_cond);
            pthread_mutex_unlock(&job_lock);
        }
    }
    return NULL;
}

long get_gcode_length(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening file");
        return -1;
    }
    long num_lines = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            num_lines++;
        }
    }
    fclose(file);
    return num_lines;
}

/* help command */
void print_usage() {
    printf("Available commands:\nsubmit <file>\nlist\nwait <jobid>\ndrain\nremove <jobid>\nhurry <jobid>\nalgo <fifo|sjf|balanced>\nquit\nhelp\n");
}

/* submit file command */
void submit(char *words[], int nwords) {

    if (nwords != 2) {
        printf("printsched: Invalid filename\n");
        return;
    }
    char* filename = words[1];

    // check if the file exists
    if (access(filename, F_OK) == -1) {
        printf("printsched: Submitted file not found\n");
        return;
    }

    // verify that filename has .gcode extension
    char* extension = strrchr(filename, '.');
    if (extension == NULL || strcmp(extension, ".gcode") != 0) {
        printf("printsched: Submitted file must be a G-code file\n");
        return;
    }

    long length = get_gcode_length(filename);
    if (length < 0) {
        fprintf(stderr, "printsched: Error reading G-code file: %s\n", filename);
        return;
    }
    
    // create a new job
    struct job_t* job = (struct job_t*)malloc(sizeof(struct job_t));
    job->id = job_id++;
    job->filename = strdup(filename);
    job->state = WAITING;
    job->submission_time = time(NULL);
    job->duration = length;
    job->pid = -1;
    job->priority = 0;

    enqueue(job_queue, job);
    printf("Job %d submitted\n", job->id);
}

/* list command */
void list() {
    pthread_mutex_lock(&job_queue->lock);
    struct job_t *curr = job_queue->head;
    struct job_t *dequeued = job_queue->dequeued_jobs;
    struct job_t *temp_list = NULL;
    int num_jobs_done = 0;
    time_t total_turnaround_time = 0, total_response_time = 0;

    // merge the dequeued jobs and the current queue
    while (dequeued != NULL || curr != NULL) {
        struct job_t *selected_job = NULL;

        if (curr == NULL) {
            selected_job = dequeued;
            dequeued = dequeued->next;
        } 
        else if (dequeued == NULL) {
            selected_job = curr;
            curr = curr->next;
        } 
        else {
            if (dequeued->id < curr->id) {
                selected_job = dequeued;
                dequeued = dequeued->next;
            } 
            else {
                selected_job = curr;
                curr = curr->next;
            }
        }

        struct job_t *new_node = (struct job_t *)malloc(sizeof(struct job_t));
        memcpy(new_node, selected_job, sizeof(struct job_t));
        new_node->next = NULL;

        if (temp_list == NULL) {
            temp_list = new_node;
        } 
        else {
            struct job_t *iter = temp_list;
            while (iter->next != NULL) {
                iter = iter->next;
            }
            iter->next = new_node;
        }
    }

    if (temp_list != NULL) {
        printf("Job ID\t\tState\t\tFilename\n");
        curr = temp_list;
        while (curr != NULL) {
            printf("%d\t\t", curr->id);
            switch (curr->state) {
                case WAITING:
                    printf("WAITING\t\t");
                    break;
                case PRINTING:
                    printf("PRINTING\t");
                    break;
                case DONE:
                    printf("DONE\t\t");
                    num_jobs_done++;
                    total_turnaround_time += curr->end_time - curr->submission_time;
                    total_response_time += curr->start_time - curr->submission_time;
                    break;
            }
            printf("%s\n", curr->filename);
            struct job_t *temp = curr;
            curr = curr->next;
            free(temp);
        }
    }

    if (num_jobs_done > 0) {
        printf("\nAverage turnaround time: %ld seconds\n", total_turnaround_time / num_jobs_done);
        printf("Average response time: %ld seconds\n", total_response_time / num_jobs_done);
    }
    pthread_mutex_unlock(&job_queue->lock);
}

/* algo command */
void change_algo_sched(algo_sched algo) {
    pthread_mutex_lock(&job_queue->lock);
    job_queue->algo = algo;
    pthread_mutex_unlock(&job_queue->lock);
}

void get_algo_sched(char *words[], int nwords) {
    if (nwords != 2) {
        printf("printsched: Error reading algorithm\n");
    }
    char *algo = words[1];
    if (strcmp(algo, "fcfs") == 0) {
        change_algo_sched(FCFS);
        printf("Scheduling algorithm set to First-Come-First-Served (FCFS)\n");
    } 
    else if (strcmp(algo, "sjf") == 0) {
        change_algo_sched(SJF);
        printf("Scheduling algorithm set to Shortest-Job-First (SJF)\n");
    } 
    else if (strcmp(algo, "balanced") == 0) {
        change_algo_sched(BALANCED);
        printf("Scheduling algorithm set to Balanced\n");
    } 
    else {
        printf("printsched: Invalid scheduling algorithm. Available algorithms: fcfs, sjf, balanced\n");
        return;
    }
}

/* hurry command */
void hurry_job(char *words[], int nwords) {
    if (nwords != 2 || !is_number(words[1])) {
        printf("printsched: Error reading hurry command\n");
    }
    int hurry_id = atoi(words[1]);

    pthread_mutex_lock(&job_queue->lock);

    struct job_t *curr = job_queue->head;
    while (curr != NULL) {
        if (curr->id == hurry_id) {
            curr->priority = 1;
            printf("Job %d marked as important\n", hurry_id);
            job_queue->hurry_count++; // increment the count of hurried jobs
            break;
        }
        curr = curr->next;
    }

    if (curr == NULL)
        printf("Job %d not found\n", hurry_id);

    pthread_mutex_unlock(&job_queue->lock);
}
/* wait command */
void wait_job(char *words[], int nwords) {
     if (nwords != 2 || !is_number(words[1])) {
        printf("printsched: Error reading wait command\n");
    }
    int wait_id = atoi(words[1]);

    pthread_mutex_lock(&job_queue->lock);
    struct job_t *curr = job_queue->head;
    struct job_t *job = NULL;
    // search for the job in job_queue
    while (curr != NULL) {
        if (curr->id == wait_id) {
            job = curr;
            break;
        }
        curr = curr->next;
    }
    // if not found, search in dequeued_jobs
    if (job == NULL) {
        curr = job_queue->dequeued_jobs;
        while (curr != NULL) {
            if (curr->id == wait_id) {
                job = curr;
                break;
            }
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&job_queue->lock);

    if (job != NULL) {
        printf("Waiting for job %d...\n", job->id);
        pthread_mutex_lock(&job_lock);
        while (job->state == PRINTING || job->state == WAITING)
            pthread_cond_wait(&job_cond, &job_lock);

        pthread_mutex_unlock(&job_lock);
        printf("Job ID: %d\n", job->id);
        printf("Job state: ");
        switch (job->state) {
            case WAITING:
                printf("WAITING\n");
                break;
            case PRINTING:
                printf("PRINTING\n");
                break;
            case DONE:
                printf("DONE\n");
                break;
        }
        printf("File: %s\n", job->filename);
        printf("Submission time: %s", ctime(&job->submission_time));
        printf("Start time: %s", ctime(&job->start_time));
        printf("End time: %s", ctime(&job->end_time));
    } 
    else 
        printf("Job ID %d not found\n", wait_id);
}

/* drain command */
void drain_queue() {
    pthread_mutex_lock(&job_queue->lock);
    if (job_queue->head != NULL) {
        printf("Draining in progress...\n");
        int all_done = 1;
        while (all_done) {
            struct job_t *curr = job_queue->head;
            if (curr == NULL) 
                break;
            while (curr != NULL) {
                if (curr->state != DONE) {
                    all_done = 0;
                    break;
                }
                curr = curr->next;
            }
            if (!all_done)
                pthread_cond_wait(&job_queue->drain_cond, &job_queue->lock);
        }
        printf("Job queue drained\n");
    } 
    else 
        printf("Job queue is empty\n");
    
    pthread_mutex_unlock(&job_queue->lock);
    return;
}

/* remove command */
void remove_job(char *words[], int nwords) {
    if (nwords != 2 || !is_number(words[1])) {
        printf("printsched: Error reading remove command\n");
    }
    int removed_id = atoi(words[1]);
    pthread_mutex_lock(&job_queue->lock);
    struct job_t *curr, *prev;
    // search for the job in dequeued_jobs
    curr = job_queue->dequeued_jobs;
    prev = NULL;
    while (curr != NULL) {
        if (curr->id == removed_id) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    // if not found in dequeued_jobs, search for the job in job_queue->head
    if (curr == NULL) {
        curr = job_queue->head;
        prev = NULL;

        while (curr != NULL) {
            if (curr->id == removed_id) {
                break;
            }
            prev = curr;
            curr = curr->next;
        }
    }

    if (curr != NULL) {
        if (curr->state == PRINTING) {
            printf("Job %d currently PRINTING state, cannot remove\n", removed_id);
        }
        else if (curr->state == DONE) {
            printf("Job %d already in DONE state\n", removed_id);
        }
        else if (curr->state == WAITING) {
            if (prev == NULL) 
                job_queue->head = curr->next;
            else 
                prev->next = curr->next;
            
            printf("Job %d removed\n", removed_id);
        }
    }
    else 
        printf("Job %d not found\n", removed_id);
    
    pthread_mutex_unlock(&job_queue->lock);
}

/* quit command */
void quit() {
    quit_flag = 1;
    pthread_mutex_lock(&job_queue->lock);
    pthread_cond_broadcast(&job_queue->queue_cond);
    pthread_mutex_unlock(&job_queue->lock);
    
    pthread_mutex_lock(&job_queue->lock);
    struct job_t *curr = job_queue->dequeued_jobs;
    while (curr != NULL) {
        if (curr->state == PRINTING) {
            printf("Killing job %d...\n", curr->id);
            if (kill(curr->pid, SIGTERM) != 0) {
                perror("kill");
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&job_queue->lock);
}

void free_job_queue() {
    pthread_mutex_lock(&job_queue->lock);
    struct job_t *curr = job_queue->head;
    struct job_t *dequeued = job_queue->dequeued_jobs;

    while (curr != NULL) {
        struct job_t *next = curr->next;
        free(curr->filename);
        free(curr);
        curr = next;
    }

    while (dequeued != NULL) {
        struct job_t *next = dequeued->next;
        free(dequeued->filename);
        free(dequeued);
        dequeued = next;
    }

    pthread_mutex_unlock(&job_queue->lock);
    pthread_mutex_destroy(&job_queue->lock);
    pthread_cond_destroy(&job_queue->queue_cond);
    pthread_cond_destroy(&job_queue->drain_cond);
    pthread_cond_destroy(&job_cond);
    free(job_queue);
}

int main(int argc, char *argv[]) {
    if (argc != 2 || !is_number(argv[1])) {
        printf("usage: printsched <number of printers>\n");
        return EXIT_SUCCESS;
    }
    int num_printer = atoi(argv[1]);

    char *input_line;
    print_usage();
    init_job_queue();

    pthread_t printer_threads[num_printer];
    printer_t thread_args = {.job_queue = job_queue};
    for (int i = 0; i < num_printer; i++) {
        if (pthread_create(&printer_threads[i], NULL, printer_thread, (void *)&thread_args)) {
        	perror("pthread_create");
        	exit(1);
    	}
    }

    while (1) {
        printf("printsched> ");
        fflush(stdout);
        input_line = calloc(4096, sizeof(char));
        if (fgets(input_line, 4096, stdin) == NULL) {
            free(input_line);
            quit();
            break;
        }

        // check if the input line is valid
        if (strlen(input_line) > BUFSIZ) {
            printf("printsched: Too many characters in the input line\n");
            continue;
        }

        /* Breaking the input line into words */
        char *words[128];
        int nwords = 0;
        int flag_break = 0;

        /* Obtain the first word */
        words[0] = strtok(input_line, " \t\n");
        if (words[0] != NULL)
            nwords++;
        else continue;

        /* Get the rest of input line */
        while ((words[nwords] = strtok(0," \t\n")) != NULL) {
            nwords++;
            // check if there are too many words
            if (nwords > sizeof(words)) {
                printf("printsched: Too many words in the input line\n");
                flag_break = 1;
                break;
            }
        }
        // continue if there are too many words
        if (flag_break)
            continue;
        
        words[nwords] = 0;

        /* Built-in commands */
        if (strcmp(words[0],"help") == 0)
            print_usage();
        else if (strcmp(words[0],"submit") == 0)
            submit(words, nwords);
        else if (strcmp(words[0],"list") == 0)
            list();
        else if (strcmp(words[0], "wait") == 0)
            wait_job(words, nwords);
        else if (strcmp(words[0], "drain") == 0)
            drain_queue();
        else if (strcmp(words[0], "hurry") == 0) 
            hurry_job(words, nwords);
        else if (strcmp(words[0], "remove") == 0) 
            remove_job(words, nwords);
        else if (strcmp(words[0], "algo") == 0)
            get_algo_sched(words, nwords);
        else if (strcmp(words[0],"quit") == 0) {
            quit();
            free(input_line);
            break;
        }
        else 
            printf("printsched: Unknown command: %s, use help command to see available commands\n", words[0]);

        free(input_line);
    }

    for (int i = 0; i < num_printer; i++) {
        if (pthread_join(printer_threads[i], NULL) != 0) 
            perror("pthread_join");
    }

    free_job_queue();
    printf("\n");
    return EXIT_SUCCESS;
}
