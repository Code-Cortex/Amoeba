#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>
#include <ctype.h>

//////////////////////////////////////////////
//              DEFINITIONS                 //
//////////////////////////////////////////////

// Set various buffer sizes and constants used throughout the program
#define LINEBUFFER 100     // Max number of tokens per observation line
#define WRDBUFFER 100      // Max length of a single word
#define SRCHMIN 1          // Minimum percentage to search database
#define SRCHMAX 100        // Maximum percentage to search database
#define CMDMAX 10          // Maximum number of arguments (words) in a generated command
#define RUNTIME 3          // Child process allowed runtime in seconds
#define NORMTHLD 250       // Threshold for normalization (not currently used)
#define REWARD 10          // Reward value when new data is learned
#define PENALTY 1          // Penalty value when redundant data is observed
#define INPUTBONUS 1       // Extra value added if input arguments appear together in observations
#define WRITEIVL 10        // Interval (in iterations) for writing the database to files

//////////////////////////////////////////////
//               GLOBALS                    //
//////////////////////////////////////////////

pid_t child_pid = 0;               // Global PID of spawned child process
volatile sig_atomic_t termination_requested = 0; // Flag to indicate if termination is requested

//////////////////////////////////////////////
//               STRUCTURES                 //
//////////////////////////////////////////////

// DatabaseStruct holds the learned words, their values, and observations
typedef struct {
    // token: an array of strings representing all known words
    // Example: token[0] might be "ls", token[1] might be "cat", etc.
    char** token;

    // value: a 4D array used to represent associations and 'value' scores between words in commands
    // value[i][j][k][l] is an int representing the value of relationship between word i at position j and word k at position l.
    // i and k: indices of words in token[]
    // j and l: indices of positions in the command
    int**** value;

    // numWords: how many words are currently in the database
    size_t numWords;

    // observation: a 2D array of observed word indices representing lines of output from executed commands
    // Each observation[i] is an int array representing a line of output tokenized into word indices found in token[].
    // observation[i][j] = index of word in token[]
    // terminated by -1 to indicate end
    int** observation;

    // numObservations: how many lines of output data we have stored
    size_t numObservations;

} DatabaseStruct;

//////////////////////////////////////////////
//            SIGNAL HANDLERS               //
//////////////////////////////////////////////

// Signal handler to set termination_requested flag if SIGINT or SIGTERM is received,
void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        termination_requested = 1;
    }
}

//////////////////////////////////////////////
//            MEMORY MANAGEMENT             //
//////////////////////////////////////////////

// Function: reallocateWords
// This function expands the token and value arrays to accommodate one new word.
// It also initializes all the associated value dimensions for that new word.
// wordLength: length of the new word being added.
void reallocateWords(DatabaseStruct* database, int wordLength) {
    char*** token = &(database->token);
    int***** value = &(database->value); // Corrected pointer type
    size_t* old_size  = &(database->numWords); 
    size_t new_size = *old_size + 1;

    if (*old_size == 0) {
        // If this is the first word:
        // Allocate token array with space for 1 word.
        *token = malloc(sizeof(char*));
        if (!*token) {
            perror("Failed to allocate memory for tokens");
            exit(EXIT_FAILURE);
        }

        // Allocate space for that 1 word
        (*token)[0] = malloc(wordLength + 1);
        if (!(*token)[0]) {
            perror("Failed to allocate memory for first token");
            exit(EXIT_FAILURE);
        }

        // Allocate the value structure:
        // value is int****, so we allocate at each dimension:
        // (*value) is an array of size new_size of int*** pointers
        *value = calloc(new_size, sizeof(int***));
        if (!*value) {
            perror("Failed to allocate memory for value array");
            exit(EXIT_FAILURE);
        }

        // Iterate through each newly allocated dimension to initialize
        for(size_t i = 0; i < new_size; i++){
            (*value)[i] = calloc(CMDMAX, sizeof(int**));
            if (!(*value)[i]) {
                fprintf(stderr, "Failed to allocate memory for value[%zu].\n", i);
                exit(EXIT_FAILURE);
            }
            for(int j = 0; j < CMDMAX; j++){
                (*value)[i][j] = calloc(new_size, sizeof(int*));
                if (!(*value)[i][j]) {
                    fprintf(stderr, "Failed to allocate memory for value[%zu][%d].\n", i, j);
                    exit(EXIT_FAILURE);
                }
                for(size_t k = 0; k < new_size; k++){
                    (*value)[i][j][k] = calloc(CMDMAX, sizeof(int));
                    if (!(*value)[i][j][k]) {
                        fprintf(stderr, "Failed to allocate memory for value[%zu][%d][%zu].\n", i, j, k);
                        exit(EXIT_FAILURE);
                    }
                }
            }
        }
    } else {
        // If we already have words and we need to add one more:
        // Increase token array size to hold one more word
        char** temp_token = realloc(*token, sizeof(char*) * new_size);
        if (!temp_token) {
            perror("Failed to realloc memory for tokens");
            exit(EXIT_FAILURE);
        }
        *token = temp_token;

        // Place the new word at index new_size-1 (zero-based indexing)
        (*token)[new_size - 1] = malloc(wordLength + 1);
        if (!(*token)[new_size - 1]) {
            perror("Failed to allocate memory for new token");
            exit(EXIT_FAILURE);
        }

        // Reallocate 'value' to hold one more word dimension:
        int**** temp_value = realloc(*value, new_size * sizeof(int***));
        if (!temp_value) {
            perror("Failed to realloc memory for value array");
            exit(EXIT_FAILURE);
        }
        *value = temp_value;

        // Now we must expand the existing arrays to accommodate the new dimensions
        for (size_t i = 0; i < new_size; i++) {
            // If this 'row' is new (i.e., i >= *old_size), allocate from scratch
            if (i >= *old_size) {
                (*value)[i] = calloc(CMDMAX, sizeof(int**));
                if (!(*value)[i]) {
                    fprintf(stderr, "Failed to allocate memory for new value[%zu].\n", i);
                    exit(EXIT_FAILURE);
                }
            }
            for (int j = 0; j < CMDMAX; j++) {
                if (i >= *old_size) {
                    // If new row in dimension i, allocate space for new_size 'k' dimension
                    (*value)[i][j] = calloc(new_size, sizeof(int*));
                    if (!(*value)[i][j]) {
                        fprintf(stderr, "Failed to allocate memory for new value[%zu][%d].\n", i, j);
                        exit(EXIT_FAILURE);
                    }
                } else {
                    // If row existed before, we need to reallocate it
                    int** temp_j = realloc((*value)[i][j], new_size * sizeof(int*));
                    if (!temp_j) {
                        fprintf(stderr, "Failed to realloc memory for value[%zu][%d].\n", i, j);
                        exit(EXIT_FAILURE);
                    }
                    (*value)[i][j] = temp_j;
                }
                for (size_t k = 0; k < new_size; k++) {
                    // If 'k' or 'i' is beyond old_size, we need to calloc a new dimension
                    if (i >= *old_size || k >= *old_size) {
                        (*value)[i][j][k] = calloc(CMDMAX, sizeof(int));
                        if (!(*value)[i][j][k]) {
                            fprintf(stderr, "Failed to allocate memory for value[%zu][%d][%zu].\n", i, j, k);
                            exit(EXIT_FAILURE);
                        }
                    }
                }
            }
        }
    }

    // Update the word count to reflect the new word
    *old_size = new_size;
}

// Function: reallocateObservations
// This function expands the observation array to accommodate one more line of observations
// Each line of observations is an array of int indices into token.
void reallocateObservations(DatabaseStruct* database, int observationLength) {
    int*** observation_ptr = &(database->observation);
    size_t* old_size = &(database->numObservations);
    size_t new_size = *old_size + 1; 

    if (*old_size == 0) {
        // No observations before, allocate the first line
        *observation_ptr = malloc(sizeof(int*) * new_size);
        if (!*observation_ptr) {
            perror("Failed to allocate memory for observations");
            exit(EXIT_FAILURE);
        }
        (*observation_ptr)[0] = malloc(sizeof(int)*(observationLength + 2));
        if (!(*observation_ptr)[0]) {
            perror("Failed to allocate memory for first observation");
            exit(EXIT_FAILURE);
        }
    } else {
        // Already have observations, so reallocate for one more line
        int** temp_observation = realloc(*observation_ptr, sizeof(int*) * new_size);
        if (!temp_observation) {
            perror("Failed to realloc memory for observations");
            exit(EXIT_FAILURE);
        }
        *observation_ptr = temp_observation;
        (*observation_ptr)[new_size - 1] = malloc(sizeof(int)*(observationLength + 2));
        if (!(*observation_ptr)[new_size - 1]) {
            perror("Failed to allocate memory for new observation");
            exit(EXIT_FAILURE);
        }
    }
    // Increment observation count
    *old_size = new_size;
}

//////////////////////////////////////////////
//               INITIALIZATION             //
//////////////////////////////////////////////

// Function: init
// Initializes the database by scanning common Linux commands from /bin and /sbin
// and adding them as words in the database.
void init(DatabaseStruct* database) {
    database->numWords = 0;
    database->numObservations = 0;
    char*** token = &(database->token);
    int***** value = &(database->value); // Corrected pointer type
    size_t* numWords = &(database->numWords);

    int chridx = 0;
    char word[WRDBUFFER];
    memset(word, 0, sizeof(word));

    // Get a list of commands by running "ls /bin && ls /sbin"
    FILE* cmdfile = popen("ls /bin && ls /sbin", "r");
    if(!cmdfile) {
        perror("popen");
        exit(EXIT_FAILURE);
    }

    int c;
    // Read characters from the pipe and split by space/newline
    while ((c = fgetc(cmdfile)) != EOF) {
        if (c != ' ' && c != '\n') {
            // Build up word until space or newline
            if (chridx < WRDBUFFER - 1) {
                word[chridx++] = (char)c;
            } else {
                // Word too long, truncate safely
                word[chridx] = '\0';
                // We'll still treat this truncated word as valid
                chridx = 0;
            }
        } else {
            // We hit a space or newline, which means we have a complete word
            if (chridx > 0) {
                word[chridx] = '\0'; // Null-terminate
                // Check if word is already known
                int redundantWord = 0;
                for (size_t i = 0; i < *numWords; i++) {
                    if ((*token)[i] && strcmp((*token)[i], word) == 0) {
                        redundantWord = 1;
                        break;
                    }
                }
                if (!redundantWord) {
                    // If new word, reallocate arrays to add this word
                    reallocateWords(database, (int)strlen(word));
                    // Copy the new word into token array at index numWords-1
                    strcpy((*token)[*numWords - 1], word);

                    // Initialize values for this new word
                    for (int i = 0; i < CMDMAX; i++) {
                        for (size_t j = 0; j < *numWords; j++) {
                            for (int k =0; k < CMDMAX; k++) {
                                if ((*numWords - 1) >= database->numWords || i >= CMDMAX || j >= database->numWords || k >= CMDMAX) {
                                    fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
                                    continue;
                                }
                                (*value)[*numWords - 1][i][j][k] = 0;
                            }
                        }
                    }
                }
                // Reset chridx for next word
                chridx = 0;
            }
        }
    }
    pclose(cmdfile);
}

//////////////////////////////////////////////
//           DATABASE PERSISTENCE           //
//////////////////////////////////////////////

// Function: writeDB
// Writes the current tokens, values, and observations to disk.
// tokens.txt for words
// values.csv for the 4D values
// observations.csv for the observation lines
void writeDB(DatabaseStruct* database) {
    FILE* tokenFile = fopen("tokens.txt", "w");
    FILE* valueFile = fopen("values.csv", "w");
    FILE* observationFile = fopen("observations.csv", "w");
    if (!tokenFile || !valueFile || !observationFile) {
        fprintf(stderr,"Failed to open files for writing.\n");
        if(tokenFile) fclose(tokenFile);
        if(valueFile) fclose(valueFile);
        if(observationFile) fclose(observationFile);
        return;
    }

    char*** token = &(database->token);
    int**** value = database->value;
    int** observation = database->observation;
    size_t numObservations = database->numObservations;
    size_t numWords = database->numWords;

    // Write each token on its own line in tokens.txt
    for (size_t i = 0; i < numWords; i++) {
        fprintf(tokenFile, "%s\n", (*token)[i]);
    }

    // Write values to values.csv
    // For each word i:
    //   For each command position j:
    //     For each word k:
    //       For each command position l:
    // Write value[i][j][k][l] separated by commas and spaces
    for (size_t i = 0; i < numWords; i++) {
        for (int j = 0; j < CMDMAX; j++) {
            for (size_t k = 0; k < numWords; k++) {
                for (int l = 0; l < CMDMAX; l++) {
                    fprintf(valueFile, "%d,", value[i][j][k][l]);
                }
                fprintf(valueFile, " ");
            }
        }
        fprintf(valueFile, "\n");
    }

    // Write observations to observations.csv
    // Each observation line ends with -1
    for (size_t i = 0; i < numObservations; i++) {
        int idx = 0;
        while (observation[i][idx] != -1) {
            fprintf(observationFile, "%d,", observation[i][idx]);
            idx++;
        }
        fprintf(observationFile, "\n");
    }

    fclose(tokenFile);
    fclose(valueFile);
    fclose(observationFile);
}

// Function: loadDB
// Attempts to load tokens, values, and observations from disk.
// If not found, returns 1 to indicate we need to init from scratch.
// Otherwise, loads everything into memory.
int loadDB(DatabaseStruct* database) {
    FILE* tokenFile = fopen("tokens.txt", "r");
    FILE* valueFile = fopen("values.csv", "r");
    FILE* observationFile = fopen("observations.csv", "r");

    char*** token = &(database->token);
    int***** value = &(database->value); // Corrected pointer type
    int*** observation_ptr = &(database->observation);
    size_t* numObservations = &(database->numObservations);
    size_t* numWords = &(database->numWords);

    *numWords = 0;
    *numObservations = 0;

    if (!tokenFile || !valueFile) {
        // No DB files found or incomplete DB: Need to init
        fprintf(stderr, "Database files not found or incomplete. Need to initialize.\n");
        if(tokenFile) fclose(tokenFile);
        if(valueFile) fclose(valueFile);
        if(observationFile) fclose(observationFile);
        return 1;
    }
    printf("Loading Database please wait...\n");

    // First load all tokens to count them
    char word[WRDBUFFER];
    while (fgets(word, sizeof(word), tokenFile)) {
        word[strcspn(word, "\n")] = '\0';
        // Check if word is already known
        int redundantWord = 0;
        for (size_t i = 0; i < *numWords; i++) {
            if ((*token)[i] && strcmp((*token)[i], word) == 0) {
                redundantWord = 1;
                break;
            }
        }
        if (!redundantWord) {
            // If new word, reallocate arrays to add this word
            reallocateWords(database, (int)strlen(word));
            // Copy the new word into token array at index numWords-1
            strcpy((*token)[*numWords - 1], word);

            // Initialize values for this new word
            for (int i = 0; i < CMDMAX; i++) {
                for (size_t j = 0; j < *numWords; j++) {
                    for (int k =0; k < CMDMAX; k++) {
                        if ((*numWords - 1) >= database->numWords || i >= CMDMAX || j >= database->numWords || k >= CMDMAX) {
                            fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
                            continue;
                        }
                        (*value)[*numWords - 1][i][j][k] = 0;
                    }
                }
            }
        }
    }

    // Now, we have loaded all words (numWords known)
    // But we need to reload them carefully to read values correctly.
    // We'll close and re-open tokenFile:
    fclose(tokenFile);
    fseek(valueFile, 0, SEEK_SET); // ensure at start of value file
    tokenFile = fopen("tokens.txt","r");
    if (!tokenFile) {
        fprintf(stderr, "Failed to reopen tokens.txt for loading values.\n");
        fclose(valueFile);
        if(observationFile) fclose(observationFile);
        return 1;
    }
    *numWords = 0; // reset and reload to align with reading values
    while (fgets(word, sizeof(word), tokenFile)) {
        word[strcspn(word, "\n")] = '\0';
        reallocateWords(database, (int) strlen(word));
        strcpy((*token)[*numWords - 1], word);

        // For the newly added word, read corresponding values from valueFile
        for (int j = 0; j < CMDMAX; j++) {
            for (size_t k = 0; k < *numWords; k++) {
                for (int l = 0; l < CMDMAX; l++) {
                    if (fscanf(valueFile, "%d,", &((*value)[*numWords - 1][j][k][l])) != 1) {
                        fprintf(stderr, "Failed to read value[%zu][%d][%zu][%d].\n", *numWords - 1, j, k, l);
                        fclose(tokenFile);
                        fclose(valueFile);
                        if(observationFile) fclose(observationFile);
                        return 1;
                    }
                }
            }
            // After reading CMDMAX values for a word, skip the space
            fscanf(valueFile, " ");
        }
        // After each word's values, skip the newline
        fscanf(valueFile, "\n");
    }

    fclose(tokenFile);
    fclose(valueFile);

    // Load observations if any
    if (observationFile) {
        char line[LINEBUFFER*4];
        while (fgets(line, sizeof(line), observationFile)) {
            int observation_line[LINEBUFFER];
            int index = 0;
            char* ptr = strtok(line, ",");
            while(ptr && index < LINEBUFFER - 1) {
                observation_line[index++] = atoi(ptr);
                ptr = strtok(NULL, ",");
            }
            observation_line[index] = -1;
            reallocateObservations(database, index);
            size_t obsIndex = *numObservations - 1;
            for(int i = 0; i <= index; i++){
                (*observation_ptr)[obsIndex][i] = observation_line[i];
            }
        }
        fclose(observationFile);
    }

    return 0;
}

//////////////////////////////////////////////
//        CHILD PROCESS MANAGEMENT         //
//////////////////////////////////////////////

// Function: check_child_status
// Waits for the child process to finish or kills it after RUNTIME seconds.
// If child is not responding, tries SIGTERM, then SIGKILL, up to 3 attempts.
int check_child_status() {
    int status;
    time_t start_time, current_time;
    time(&start_time);
    int kill_attempts = 0;

    while (1) {
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);
        time(&current_time);
        int proc_time = (int)difftime(current_time, start_time);

        if (proc_time >= RUNTIME) {
            // Child ran too long, try to kill it
            if (kill_attempts == 0) {
                if (kill(child_pid, SIGTERM) != 0) {
                    perror("kill SIGTERM");
                }
            } else if (kill_attempts == 1) {
                if (kill(child_pid, SIGKILL) != 0) {
                    perror("kill SIGKILL");
                }
            } else if (kill_attempts == 2) {
                fprintf(stderr, "Failed to terminate the child process %d.\n", child_pid);
                child_pid = 0;
                return 1;
            }
            sleep(1);
            kill_attempts++;
        } else if (wpid == child_pid) {
            // wpid matches child_pid, so child changed state
            if (WIFEXITED(status)) {
                child_pid = 0;
                return 0;
            }
            if (WIFSIGNALED(status)) {
                child_pid = 0;
                return 0;
            }
        } else if (wpid < 0) {
            perror("waitpid");
            child_pid = 0;
            return 1;
        }
        // Short sleep to prevent busy waiting
        usleep(100000); // 100 ms
    }
}

//////////////////////////////////////////////
//        COMMAND CONSTRUCTION (ML)         //
//////////////////////////////////////////////

// Function: constructCommand
// Attempts to construct a command array of length cmdlen from the learned words.
// srchpct indicates the percentage of the word database to search.
// This function tries to find a command combination with improved 'value'.
int* constructCommand(DatabaseStruct* database, int cmdlen, int srchpct) {
    // Extract shortcuts to data
    size_t numWords = database->numWords;
    size_t numObservations = database->numObservations;
    int**** value = database->value;
    int** obs = database->observation;

    // Limit cmdlen to CMDMAX - 1
    if (cmdlen >= CMDMAX) cmdlen = CMDMAX -1;

    // srchitr is how many times we attempt random improvements
    int srchitr = (int)((numWords * srchpct) / 100);
    if (srchitr < 1) srchitr = 1; // Ensure at least one iteration

    // Local variables for searching best command combination
    int select = 0;
    int cmdval = 0;
    int prevcmdval = 0;
    int arg1found = 0;
    int arg2found = 0;

    // cmdint will hold the indices of words chosen for the command
    // static to avoid returning pointer to local variable
    static int cmdint[CMDMAX + 1];
    for(int i = 0; i < CMDMAX + 1; i++) cmdint[i] = -1;

    srand((unsigned)time(NULL));

    // For simplicity, the code tries multiple iterations (srchitr) 
    // and tries to improve the command value by changing arguments
    for (int i = 0; i < srchitr; i++) {
        for (int j = 0; j < cmdlen; j++) {
            // Randomly select a word
            if (numWords == 0) {
                // No words available to construct command
                return cmdint;
            }
            select = (int)(rand() % numWords);
            if (i == 0) {
                // First iteration builds a baseline command
                cmdint[j] = select;
                if (j == cmdlen-1) {
                    // Once the command is fully chosen, calculate its initial score
                    for (int k = 0; k < cmdlen; k++) {
                        for (int l = 0; l < cmdlen; l++) {
                            if (cmdint[k] < 0 || cmdint[l] < 0 || (size_t)cmdint[k] >= numWords || (size_t)cmdint[l] >= numWords) {
                                continue;
                            }
                            prevcmdval += value[cmdint[k]][k][cmdint[l]][l];
                            // Also check observations to add INPUTBONUS if args appear together
                            for (size_t m = 0; m < numObservations; m++){
                                arg1found = 0; arg2found = 0;
                                for (int n = 0; obs[m][n] != -1; n++) {
                                    if (obs[m][n] == select) {
                                        arg1found = 1;
                                    }
                                    if (obs[m][n] == cmdint[k]) {
                                        arg2found = 1;
                                    }
                                }
                                if (arg1found && arg2found) {
                                    prevcmdval += INPUTBONUS;
                                }
                            }
                        }
                    }
                }
            } else {
                // Subsequent iterations: try changing one argument and see if it improves
                cmdval = 0;
                for (int k = 0; k < cmdlen; k++) {
                    if (select < 0 || cmdint[k] < 0 || (size_t)select >= numWords || (size_t)cmdint[k] >= numWords) {
                        continue;
                    }
                    cmdval += value[select][j][cmdint[k]][k];
                    // Check observations again
                    for (size_t l = 0; l < numObservations; l++){
                        arg1found = 0; arg2found = 0;
                        for (int m = 0; obs[l][m] != -1; m++) {
                            if (obs[l][m] == select) {
                                arg1found = 1;
                            }
                            if (obs[l][m] == cmdint[k]) {
                                arg2found = 1;
                            }
                        }
                        if (arg1found && arg2found) {
                            cmdval += INPUTBONUS;
                        }
                    }
                }
                // If the new cmdval is better than prevcmdval, update the command
                if (cmdval > prevcmdval) {
                    cmdint[j] = select;
                    prevcmdval = cmdval;
                    cmdval = 0;
                }
            }
        }
    }

    cmdint[cmdlen] = -1; // Terminate command list
    return cmdint;
}

//////////////////////////////////////////////
//          EXECUTE COMMAND & CAPTURE       //
//////////////////////////////////////////////

// Function: executeCommand
// Executes the given command string using /bin/sh, and captures its output (stdout/stderr) into a string.
// Returns the output as a dynamically allocated char*.
char* executeCommand(char cmd[]) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return NULL;
    }

    // Fork a child to run the command
    if ((child_pid = fork()) == 0) {
        // Child process: redirect stdout & stderr into pipe
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)0);
        perror("execl");
        exit(1);
    } else if (child_pid < 0) {
        // fork failed
        perror("fork");
        return NULL;
    }

    // Parent: close write end, read from pipe
    close(pipefd[1]);
    char* output = malloc(1);
    if (!output) {
        perror("Failed to allocate memory for command output");
        close(pipefd[0]);
        return NULL;
    }
    int output_index = 0;
    char current_char;
    while (read(pipefd[0], &current_char, 1) > 0) {
        if (current_char == '\n' || isprint((unsigned char)current_char)) {
            char* temp = realloc(output, sizeof(char) * (output_index + 2));
            if (!temp) {
                perror("Failed to realloc memory for command output");
                free(output);
                close(pipefd[0]);
                return NULL;
            }
            output = temp;
            output[output_index++] = current_char;
        }
    }
    close(pipefd[0]);
    output = realloc(output, sizeof(char) * (output_index + 1));
    if (!output) {
        perror("Failed to realloc memory for final command output");
        return NULL;
    }
    output[output_index] = '\0';
    return output;
}

//////////////////////////////////////////////
//          UPDATE DATABASE (LEARNING)      //
//////////////////////////////////////////////

// Function: updateDatabase
// Given the output of a command and the command indices (cmdint),
// updates the database with any new words found in output, and rewards/penalizes accordingly.
int updateDatabase(DatabaseStruct* database, char* output, int* cmdint) {
    // Correctly declare value as a pointer to the value field
    int***** value = &(database->value); // Pointer to int**** (i.e., int*****)

    char*** token = &(database->token);
    int*** observations = &(database->observation);
    size_t* numObservations = &database->numObservations;
    size_t* numWords = &database->numWords;

    int chridx = 0;
    int lrnval = 0;
    int output_index = 0;
    int observationLength = 0;
    int tokenized_line[LINEBUFFER]; // Buffer to hold token indices of one observation line
    char word[WRDBUFFER];
    memset(word, 0, sizeof(word));

    // Parse the output into words
    while (1) {
        char c = output[output_index];
        if (c == '\0') {
            // End of output, handle any last word
            if (chridx > 0) {
                word[chridx] = '\0';
                chridx = 0;

                // Check if the word is already known
                int redundantWord = 0;
                size_t w;
                for (w = 0; w < *numWords; w++) {
                    if (strcmp((*token)[w], word) == 0) {
                        redundantWord = 1;
                        break;
                    }
                }

                // If new word
                if (!redundantWord && strlen(word) != 0) {
                    reallocateWords(database, (int)strlen(word));
                    // Now *numWords has been incremented inside reallocateWords
                    strcpy((*token)[*numWords - 1], word);

                    // Initialize value for new word
                    for (int ii = 0; ii < CMDMAX; ii++) {
                        for (size_t jj = 0; jj < *numWords; jj++) {
                            for (int kk = 0; kk < CMDMAX; kk++) {
                                if ((*numWords - 1) >= database->numWords || ii >= CMDMAX || jj >= database->numWords || kk >= CMDMAX) {
                                    fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
                                    continue;
                                }
                                (*value)[*numWords - 1][ii][jj][kk] = 0;
                            }
                        }
                    }
                }

                // Add the token index for the last word to tokenized_line
                for (w = 0; w < *numWords; w++) {
                    if (strcmp((*token)[w], word) == 0) {
                        tokenized_line[observationLength] = (int)w;
                        observationLength++;
                        break;
                    }
                }
            }
            break; // no more chars in output
        } else if (c == ' ' || c == '\n') {
            // End of a word
            if (chridx > 0) {
                word[chridx] = '\0';
                chridx = 0;

                // Check if this word is new
                int redundantWord = 0;
                size_t w;
                for (w = 0; w < *numWords; w++) {
                    if (strcmp((*token)[w], word) == 0) {
                        redundantWord = 1;
                        break;
                    }
                }
                if (!redundantWord && strlen(word) != 0) {
                    reallocateWords(database, (int)strlen(word));
                    strcpy((*token)[*numWords - 1], word);

                    for (int ii = 0; ii < CMDMAX; ii++) {
                        for (size_t jj = 0; jj < *numWords; jj++) {
                            for (int kk = 0; kk < CMDMAX; kk++) {
                                if ((*numWords - 1) >= database->numWords || ii >= CMDMAX || jj >= database->numWords || kk >= CMDMAX) {
                                    fprintf(stderr, "Index out of bounds while initializing value for new word.\n");
                                    continue;
                                }
                                (*value)[*numWords - 1][ii][jj][kk] = 0;
                            }
                        }
                    }
                }

                // Add this token's index to tokenized_line
                for (w = 0; w < *numWords; w++) {
                    if (strcmp((*token)[w], word) == 0) {
                        tokenized_line[observationLength] = (int)w;
                        observationLength++;
                        break;
                    }
                }

                // If we hit a newline, that means we completed one observation line
                if (c == '\n') {
                    // Check if this observation line is new
                    int redobs = 0;
                    for (size_t line = 0; line < *numObservations; line++) {
                        int match = 1;
                        for (int idx = 0; idx < observationLength; idx++) {
                            if ((*observations)[line][idx] != tokenized_line[idx]) {
                                match = 0;
                                break;
                            }
                        }
                        // Check if ended exactly with -1
                        if (match && ((*observations)[line][observationLength] == -1)) {
                            redobs = 1;
                            break;
                        }
                    }

                    if (!redobs) {
                        // New observation line
                        lrnval += REWARD;
                        // Allocate space for a new observation line
                        reallocateObservations(database, observationLength);
                        // reallocateObservations increments *numObservations internally

                        // Now write the tokenized_line into observations
                        // *numObservations is now incremented, so we use (*numObservations - 1)
                        size_t obsIndex = *numObservations - 1;
                        for (int i = 0; i < observationLength; i++) {
                            (*observations)[obsIndex][i] = tokenized_line[i];
                        }
                        // Add the terminator
                        (*observations)[obsIndex][observationLength] = -1;
                    } else {
                        // Redundant observation line
                        lrnval -= PENALTY;
                    }

                    // Reset for the next line
                    observationLength = 0;
                }
            }
        } else {
            // Building a word
            if (chridx < WRDBUFFER - 1) {
                word[chridx++] = c;
            } else {
                // Word too long, truncate safely
                word[chridx] = '\0';
                // We'll still treat this truncated word as valid
                chridx = 0;
            }
        }
        output_index++;
    }

    // After processing all output
    // If any observation line is incomplete (no newline), we ignore it or handle accordingly:
    // If we consider incomplete lines as not valid observations, do nothing.

    // Update values for the command arguments (if necessary)
    for (int i = 0; cmdint[i] != -1; i++) {
        for (int j = 0; cmdint[j] != -1; j++) {
            // Ensure cmdint[i], cmdint[j] are within range [0, *numWords-1]
            if (cmdint[i] >= 0 && (size_t)cmdint[i] < *numWords && 
                cmdint[j] >= 0 && (size_t)cmdint[j] < *numWords) {
                (*value)[cmdint[i]][i][cmdint[j]][j] += lrnval;
            } else {
                fprintf(stderr, "Invalid cmdint indices: cmdint[%d]=%d, cmdint[%d]=%d\n", 
                        i, cmdint[i], j, cmdint[j]);
            }
        }
    }
    return lrnval;
}

//////////////////////////////////////////////
//             CLEANUP FUNCTION             //
//////////////////////////////////////////////

// Function: cleanup
// Frees all dynamically allocated memory to prevent leaks.
void cleanup(DatabaseStruct* database) {
    // Free token arrays
    if (database->token) {
        for (size_t i = 0; i < database->numWords; i++){
            free(database->token[i]);
        }
        free(database->token);
    }

    // Free observation arrays
    if (database->observation) {
        for (size_t i = 0; i < database->numObservations; i++){
            free(database->observation[i]);
        }
        free(database->observation);
    }

    // Free value arrays (4D structure)
    if (database->value) {
        for (size_t i = 0; i < database->numWords; i++) {
            for (int j = 0; j < CMDMAX; j++) {
                for (size_t k = 0; k < database->numWords; k++) {
                    free(database->value[i][j][k]);
                }
                free(database->value[i][j]);
            }
            free(database->value[i]);
        }
        free(database->value);
    }
}

//////////////////////////////////////////////
//                 MAIN                     //
//////////////////////////////////////////////

int main() {
    // Learning value tracking
    int lrnval = 0;
    int prevlrnval = 0;

    // srchpct is the percentage of words to search when constructing commands
    int srchpct = 1;

    // cmdlen is the length of the constructed command
    int cmdlen = 1;

    // writeCount counts how many commands executed before writing DB
    int writeCount = 0;

    // Redundancy checking variables
    int prevRedundancy = 0;

    // Command buffer for execution
    char cmd[WRDBUFFER * CMDMAX];
    memset(cmd, 0, sizeof(cmd));

    // Initialize database
    DatabaseStruct database;
    database.token = NULL;
    database.value = NULL;
    database.observation = NULL;
    database.numWords = 0;
    database.numObservations = 0;

    // Setup signal handling for graceful termination
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Attempt to load database from disk. If not present, init from scratch.
    int chkinit = loadDB(&database);
    if(chkinit) {
        printf("Initializing please wait...\n");
        init(&database);
    }

    int prevCmdint[CMDMAX + 1];
    for (int i = 0; i < CMDMAX + 1; i++) prevCmdint[i] = -1;

    // Main loop: Continues until termination_requested is set
    while (!termination_requested) {
        int* cmdint = constructCommand(&database, cmdlen, srchpct);

        if (cmdint == NULL) {
            fprintf(stderr, "Failed to construct command. Skipping iteration.\n");
            continue;
        }

        if (prevCmdint[0] == -1) {
            // First run, just copy current cmdint to prevCmdint
            for (int i = 0; i < CMDMAX + 1; i++) {
                prevCmdint[i] = cmdint[i];
            }
        } else {
            // Check redundancy with previous command
            int currentRed = 0;
            for (int i = 0; cmdint[i] != -1 && prevCmdint[i] != -1; i++) {
                if (cmdint[i] == prevCmdint[i]) {
                    currentRed++;
                }
            }
            // Adjust srchpct based on redundancy
            if (currentRed > prevRedundancy && srchpct > SRCHMIN) {
                srchpct--;
            } else {
                srchpct++;
                if (srchpct > SRCHMAX) srchpct = SRCHMAX;
            }
            prevRedundancy = currentRed;

            for (int i = 0; i < CMDMAX + 1; i++) {
                prevCmdint[i] = cmdint[i];
            }
        }

        // Build command string from cmdint
        cmd[0] = '\0';
        for (int i = 0; cmdint[i] != -1; i++) {
            if (cmdint[i] >= 0 && (size_t)cmdint[i] < database.numWords) {
                strcat(cmd, database.token[cmdint[i]]);
                strcat(cmd, " ");
            } else {
                fprintf(stderr, "Invalid cmdint[%d] = %d. Skipping this word.\n", i, cmdint[i]);
            }
        }

        // Trim trailing space
        size_t len = strlen(cmd);
        if (len > 0 && cmd[len - 1] == ' ') {
            cmd[len - 1] = '\0';
        }

        printf("\n$ %s\n", cmd);
        char* output = executeCommand(cmd);
        if (output != NULL){
            printf("%s", output);
            lrnval = updateDatabase(&database, output, cmdint);
            free(output);

            // If we learned more or at least not worse than before, maybe increase cmdlen randomly
            if (lrnval >= prevlrnval) {
                if (cmdlen < CMDMAX) {
                    cmdlen += (rand() % 2); 
                    if (cmdlen >= CMDMAX) cmdlen = CMDMAX - 1;
                }
            } else if (cmdlen > 1) {
                // If we learned less than before, try shorter commands
                cmdlen -= (rand() % 2);
                if (cmdlen < 1) cmdlen = 1;
            }
            prevlrnval = lrnval;
            writeCount++;
        }
        
        // Write database to files periodically
        if (writeCount == WRITEIVL) {
            writeDB(&database);
            writeCount = 0;
        }
    }

    // Cleanup all dynamically allocated memory on exit
    cleanup(&database);
    printf("\n\nProgram exited successfully!\n");
    return 0;
}
