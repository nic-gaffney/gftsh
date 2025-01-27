#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Define the prompt
#define PROMPT                                                                 \
    ("\x1b[1m\x1b[31m\\u\x1b[32m@"                                             \
     "\x1b[34m\\h \x1b[35m\\w\x1b[0m\x1b[33m "                                 \
     "$\x1b[0m ")
#define WHITESPACE " \t\n"

typedef char **Command;

char *promptparse(char *prompt, char *buf);
int splitby(char *STR, char **BUF, const char *DELIM);
glob_t globbize(char **STRS);
int genpipeline(char *CMD, Command *PIPELINE);
void runpipeline(int IN, int OUT, Command *PIPELINE, int n);
int special_commands(char **ARGV, pid_t ppid);
void run_cmd(int IN, int OUT, Command COMMAND);
void sigint_handler();
void sigalrm_handler();

int main(int argc, char *args[]) {
    (void)argc; // just so the compiler would shut up
    // Initialize buf to all zeros
    setenv("SHELL", args[0], 1);
    setenv("PS1", PROMPT, 0);
    char prompt[256];

    pid_t ppid = getpid();
    size_t size = 1024;
    char buf[size];
    char *cmds[64];
    memset(buf, 0, size);
    fputs(promptparse(getenv("PS1"), prompt), stdout);
    signal(SIGINT, sigint_handler); // ^C wont crash the shell
    while (fgets(buf, size, stdin)) {
        int len = splitby(buf, cmds, ";"); // Split by ;
        for (int i = 0; i < len; i++) {
            // Handle exit and cd
            char *specialbuf[64];
            char tmpstr[size];
            strncpy(tmpstr, cmds[i], size);
            splitby(tmpstr, specialbuf, WHITESPACE);
            int status = special_commands(specialbuf, ppid);
            if (status == 2)
                exit(0);
            if (status)
                continue;
            // Run command
            pid_t parent_pid = getpid();
            pid_t pid = fork();
            if (pid == 0) {
                char **pipeline[64];
                int len = genpipeline(cmds[i], pipeline);
                runpipeline(STDIN_FILENO, STDOUT_FILENO, pipeline, len);
                if (status == 2)
                    kill(parent_pid, SIGUSR1);
                exit(0);
            } else
                wait(&pid);
        }
        fputs(promptparse(getenv("PS1"), prompt), stdout);
    }
    // EOF in STDOUT
    fputs("\nexit\n", stdout);
    return 0;
}

// SIGALRM handler
void sigalrm_handler() { exit(0); }

// Ignore SIGINT and print newline
void sigint_handler() {
    char prompt[256];
    promptparse(getenv("PS1"), prompt);
    write(STDOUT_FILENO,
          "\n ** This is the message from week09 lab2 - Signal Handler! "
          "**\n",
          63);
    write(STDOUT_FILENO, prompt, strlen(prompt));
}

/* Take NULL terminated array of strings STRS and glob them.
 * Caller must remember to run `glob_free()` on the resulting
 * glob_t value. */
glob_t globbize(char **strs) {
    glob_t globbuf;
    int FLAGS = GLOB_NOCHECK | GLOB_TILDE;
    glob(strs[0], FLAGS, NULL, &globbuf);
    for (int i = 1; strs[i]; i++)
        glob(strs[i], FLAGS | GLOB_APPEND, NULL, &globbuf);

    return globbuf;
}

/* Take input string STR and split it by DELIM
 * into an array of strings stored in BUF.
 * If BUF is not large enough to hold the entire command, the program
 * will segfault. */
int splitby(char *str, char **buf, const char *delim) {
    int count = 0;
    buf[count] = strtok(str, delim);
    while ((buf[++count] = strtok(NULL, delim)))
        ;
    return count;
}

/* Run null terminated command ARGV, where the first element is
 * the command and all following elements are arguments.
 * Uses `execvp`, and thus has access to PATH environment
 * variable. Requires SHELLNAME for error reporting. */
void run_cmd(int in, int out, char **argv) {
    char *shellname = getenv("SHELL");
    pid_t pid;
    int status;
    if ((pid = fork()) < 0) {
        fprintf(stderr, "%s: failed to fork: %s\n", shellname, strerror(errno));
        exit(1);
    }
    if (pid != 0) {
        while (wait(&status) != pid)
            ;
        return;
    }
    // Child process
    if (in != STDIN_FILENO) {
        dup2(in, STDIN_FILENO);
        close(in);
    }
    if (out != STDOUT_FILENO) {
        dup2(out, STDOUT_FILENO);
        close(out);
    }
    if (execvp(argv[0], argv) < 0) {
        fprintf(stderr, "%s: %s: %s\n", shellname, argv[0],
                errno == ENOENT ? "command not found" : strerror(errno));
        exit(1);
    }
}

/* Check if command ARGV is a special command, and if so run it.
 * Returns a 1 upon executing a special command, and 0 if no special
 * command was found.
 * Special commands include: { exit , cd <dirname> }
 */
int special_commands(char **argv, pid_t ppid) {
    if (!argv[0])
        return 0;
    if (!strcmp("exit", argv[0]))
        return 2;
    if (!strcmp("alarm", argv[0])) {
        int secs = strtol(argv[1], NULL, 10);
        if (secs)
            printf("Alarm set! %d secs\n", secs);
        else
            printf("Alarm off!\n");
        alarm(secs);
        return 1;
    }
    if (!strcmp("cd", argv[0])) {
        if (!argv[1]) {
            chdir(getpwuid(getuid())->pw_dir);
            return 1;
        }
        if (chdir(argv[1]) == -1)
            fprintf(stderr, "%s: cd: %s: %s\n", getenv("SHELL"), argv[1],
                    strerror(errno));
        return 1;
    }
    return 0;
}

/* Takes input, containing > or <, and returns fds (int[2]) containing
 * the file descriptors for a redirected input and output.
 * fds contains { -1, -1 } by default and thus non positive values should
 * be ignored.
 */
int *redirect(char **input) {
    int *fds = malloc(sizeof(int[2]));
    fds[0] = -1;
    fds[1] = -1;
    const mode_t FLAGS = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    for (int i = 0; input[i]; i++) {
        if (!strncmp("<", input[i], 1)) {
            fds[0] = open(input[++i], O_RDONLY, FLAGS);
            input[i - 1] = NULL;
            break;
        }
        if (!strncmp(">", input[i], 1)) {
            fds[1] = open(input[++i], O_WRONLY | O_CREAT | O_TRUNC, FLAGS);
            input[i - 1] = NULL;
            break;
        }
    }
    return fds;
}

/* Runs a given PIPELINE (Command*)  */
void runpipeline(int in, int out, Command *pipeline, int n) {
    int i = 0;
    int fds[2];
    int *redirs = redirect(pipeline[0]);
    if (redirs[0] != -1)
        dup2(redirs[0], in);
    if (n != 1) {
        for (i = 0; i < n - 1; i++) {
            pipe(fds);
            run_cmd(in, fds[1], pipeline[i]);
            close(fds[1]);
            in = fds[0];
        }
        free(redirs);
        redirs = redirect(pipeline[i]);
    }
    if (in != STDIN_FILENO)
        dup2(in, STDIN_FILENO);
    if (redirs[1] != -1)
        dup2(redirs[1], out);
    run_cmd(in, out, pipeline[i]);
    if (redirs[0] != -1)
        close(redirs[0]);
    if (redirs[1] != -1)
        close(redirs[1]);
    free(redirs);
}

/* Takes string CMD and splits it into PIPELINE.
 * Results in an array of commands (char**).
 * Pass into `runpipeline` to run the pipeline.
 */
int genpipeline(char *cmd, char ***pipeline) {
    char *cmds[64];
    int pipelen = splitby(cmd, cmds, "|");
    for (int i = 0; i < pipelen; i++) {
        char **argv = malloc(sizeof(char *[64]));
        splitby(cmds[i], argv, WHITESPACE);
        glob_t globbuf = globbize(argv);
        pipeline[i] = globbuf.gl_pathv;
        free(argv);
    }
    return pipelen;
}

// Not my best work.
char *promptparse(char *prompt, char *ret) {
    memset(ret, 0, 256);
    int i, j;
    char buf[128];

    const char *username = getenv("USER");
    if (username == NULL) {
        username = getenv("LOGNAME");
    }
    const char *home = getenv("HOME");
    if (home == NULL)
        home = getpwuid(getuid())->pw_dir;

    char hostname[128];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname");
        hostname[0] = '\0'; // Set hostname to empty string on error
    }

    for (i = 0, j = 0; prompt[i]; i++, j++) {
        switch (prompt[i]) {
        case '\\':
            switch (prompt[++i]) {
            case 'w':
                if (getcwd(buf, sizeof(buf)) == NULL)
                    break;
                j += -1 + snprintf(ret + j, 256 - j,
                                   strncmp(buf, home, strlen(home)) == 0 /**/
                                       ? "~%s"
                                       : "%s",
                                   strncmp(buf, home, strlen(home)) == 0 /**/
                                       ? buf + strlen(home)
                                       : buf);

                break;
            case 'u':
                j += -1 + snprintf(ret + j, 256 - j, "%s",
                                   username ? username : "unknown");
                break;
            case 'h':
                j += -1 + snprintf(ret + j, 256 - j, "%s", hostname);
                break;
            }
            break;
        default:
            ret[j] = prompt[i];
            break;
        }
    }
    ret[j] = '\0';
    return ret;
}
