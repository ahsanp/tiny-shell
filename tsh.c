/*
 * tsh - A tiny shell program with job control
 *
 * Ahsan Pervaiz
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv, pid_t *pid);
void do_bgfg(char **argv, pid_t *pid);
void waitfg(pid_t pid);

pid_t Fork();
void Sigfillset(sigset_t *mask);
void Sigemptyset(sigset_t *mask);
void Sigaddset(sigset_t *mask, int signo);
void Sigprocmask(int action, sigset_t *mask, sigset_t *prev_mask);
void Write(int fd, const void *buf, size_t count);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
        break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
        break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
        break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {
        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
*/
void eval(char *cmdline)
{
    char *argv[MAXARGS];
    int bg_flag;
    pid_t pid;
    sigset_t mask_sigchld, mask_all, prev_mask;
    Sigfillset(&mask_all);
    Sigemptyset(&mask_sigchld);
    Sigaddset(&mask_sigchld, SIGCHLD);
    bg_flag = parseline(cmdline, argv);
    if (!builtin_cmd(argv, &pid)) {
        Sigprocmask(SIG_BLOCK, &mask_sigchld, &prev_mask); // block SIGCHLD
        if ((pid = Fork()) == 0) {
            // Run in the child process
            // reset all masks in child process
            setpgid(0, 0);
            Sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            execve(argv[0], argv, environ);
        }
        Sigprocmask(SIG_BLOCK, &mask_all, NULL); // block all signals
        addjob(jobs, pid, (bg_flag) ? BG : FG, cmdline);
        if (bg_flag) {
            char buf[MAXLINE + 10];
            sprintf(buf, "[%d] (%d) %s", pid2jid(pid), pid, cmdline);
            Write(STDOUT_FILENO, buf, strlen(buf));
        }
        Sigprocmask(SIG_SETMASK, &prev_mask, NULL); // reset all signals
    }
    waitfg(pid); // wait if a process is in foreground
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
               buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    if (argc == 0)  /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
        argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv, pid_t *pid)
{
    if (!strcmp(argv[0], "quit")) {
        exit(0);
    } else if (!strcmp(argv[0], "jobs")) {
        listjobs(jobs);
        return 1;
    } else if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")) {
        do_bgfg(argv, pid);
        return 1;
    }
    return 0;
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv, pid_t *pid)
{
    char buf[MAXLINE + 20];
    int jid;
    sigset_t mask_all, prev_mask;
    sscanf(argv[1], "%%%d", &jid);
    Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
    struct job_t *job = getjobjid(jobs, jid);
    *pid = job -> pid;
    if (kill(*pid, SIGCONT) < 0) {
        unix_error("Could not send continue signal process");
    }
    if (!strcmp(argv[0], "bg")) {
        job -> state = BG;
        sprintf(buf, "[%d] (%d) %s", jid, *pid, job -> cmdline);
        Write(STDOUT_FILENO, buf, strlen(buf));
    } else {
        job -> state = FG;
    }
    Sigprocmask(SIG_SETMASK, &prev_mask, NULL);
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    sigset_t mask_none;
    Sigemptyset(&mask_none);
    while (pid2jid(pid) && pid == fgpid(jobs))
        sigsuspend(&mask_none);
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig)
{
    int prev_error, jid, status;
    pid_t pid;
    char buf[1024];
    prev_error = errno;
    sigset_t mask_all, prev_mask;
    Sigfillset(&mask_all);
    // reap all zombies
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        jid = pid2jid(pid);
        if (WIFSIGNALED(status)) {
            // write message if terminated by an uncaught signal
            sprintf(buf, "Job [%d] (%d) terminated by signal %d\n",
                    jid, pid, WTERMSIG(status));
            Write(STDOUT_FILENO, buf, strlen(buf));
        } else if (WIFSTOPPED(status)) {
            // write message if the process was stopped by a signal
            sprintf(buf, "Job [%d] (%d) stopped by signal %d\n",
                    jid, pid, WSTOPSIG(status));
            Write(STDOUT_FILENO, buf, strlen(buf));
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            // delete process from the job list
            // if terminated normally or by SIGINT
            Sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
            deletejob(jobs, pid);
            Sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        }
    }
    errno = prev_error; // restore errno
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
    int prev_err = errno;
    sigset_t mask_all, prev_mask;
    Sigfillset(&mask_all);
    pid_t pid;
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
    if ((pid = fgpid(jobs)) != 0) {
        if (kill(-pid, SIGINT) < 0) {
            unix_error("Problem sending signal");
        }
    }
    Sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    errno = prev_err;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    int prev_err = errno;
    sigset_t mask_all, prev_mask;
    Sigfillset(&mask_all);
    pid_t pid;
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
    if ((pid = fgpid(jobs)) != 0) {
        if (kill(-pid, SIGTSTP) < 0) {
            unix_error("Problem sending stop signal");
        }
        struct job_t *job = getjobpid(jobs, pid);
        job -> state = ST;
    }
    Sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    errno = prev_err;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
        max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
    return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
            nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
                return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
    return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
    return NULL;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
        return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
    int i;

    if (jid < 1)
    return NULL;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
        return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
    return 0;
    for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
            case BG:
                printf("Running ");
                break;
            case FG:
                printf("Foreground ");
                break;
            case ST:
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ",
                   i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

/*
 * Fork - wrapper for fork function
 *    inclue error handling
 */
pid_t Fork() {
    pid_t pid;
    if ((pid = fork()) < 0) {
        unix_error("Could not fork child process");
    }
    return pid;
}

/*
 * Sigfillset - wrapper for the sigfillset function
 */
void Sigfillset(sigset_t *mask) {
    if (sigfillset(mask) < 0) {
        unix_error("Could not fill mask");
    }
}

/*
 * Sigemptyset - wrapper for the sigemptyset function
 */
void Sigemptyset(sigset_t *mask) {
    if (sigemptyset(mask) < 0) {
        unix_error("Could not empty mask");
    }
}

/*
 * Sigaddset - wrapper for the sigaddset function
 */
void Sigaddset(sigset_t *mask, int signo) {
    if (sigaddset(mask, signo) < 0) {
        unix_error("Could not add to set");
    }
}

/*
 * Sigprocmask - wrapper for the sigprocmask function
 */
void Sigprocmask(int action, sigset_t *mask, sigset_t *prev_mask) {
    if (sigprocmask(action, mask, prev_mask) < 0) {
        unix_error("Could not perform action on mask");
    }
}

/*
 * Write - error handling wrapper for write system call
 */
void Write(int fd, const void *buf, size_t count) {
    if (write(fd, buf, count) < 0) {
        unix_error("Error making write system call");
    }
}
