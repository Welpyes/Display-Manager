#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

// Constants for layout
#define MIN_ROWS 10
#define MIN_COLS 40
#define TITLE_ROW_OFFSET -4
#define SEPARATOR_ROW_OFFSET -3
#define USER_ROW_OFFSET -2
#define PROMPT_ROW_OFFSET 0
#define ERROR_ROW_OFFSET 3
#define PROMPT_COL_FACTOR 0.25
#define MAX_PASSWORD_LEN 30
#define ERROR_TIMEOUT 2 // Seconds to show errors

// Config struct for .dmrc data
typedef struct {
    char username[256];
    char pwd[256];
    char cmd[512];
} Config;

// Global for signal handling
static volatile int running = 1;
static volatile int resized = 0;

// Signal handlers
void handle_sigint(int sig) {
    running = 0;
}

void handle_sigwinch(int sig) {
    resized = 1;
}

// Read ~/.dmrc file
int read_dmrc(Config *cfg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.dmrc", getenv("HOME"));
    FILE *file = fopen(path, "r");
    if (!file) return -1;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '\n' || *start == '#') continue;

        char *eq = strchr(start, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = start;
        char *value = eq + 1;

        while (key[strlen(key)-1] == ' ' || key[strlen(key)-1] == '\t')
            key[strlen(key)-1] = '\0';
        while (*value == ' ' || *value == '\t') value++;
        value[strcspn(value, "\n")] = '\0';

        if (!strcmp(key, "username")) strncpy(cfg->username, value, sizeof(cfg->username)-1);
        else if (!strcmp(key, "pwd")) strncpy(cfg->pwd, value, sizeof(cfg->pwd)-1);
        else if (!strcmp(key, "cmd")) strncpy(cfg->cmd, value, sizeof(cfg->cmd)-1);
    }
    fclose(file);
    return 0;
}

// Center text in a window
void print_centered(WINDOW *win, int row, int width, const char *text, int color_pair) {
    int len = strlen(text);
    int col = (width - len) / 2;
    wattron(win, color_pair);
    mvwprintw(win, row, col, "%s", text);
    wattroff(win, color_pair);
}

// Draw the UI
void draw_ui(WINDOW *win, int rows, int cols, Config *cfg, const char *error_msg, int input_pos) {
    werase(win);
    box(win, 0, 0); // Draw border

    // Title and separator
    print_centered(win, rows/2 + TITLE_ROW_OFFSET, cols, "Display Manager", COLOR_PAIR(1));
    print_centered(win, rows/2 + SEPARATOR_ROW_OFFSET, cols, "---------------------", 0);

    // Username
    char user_line[512];
    snprintf(user_line, sizeof(user_line), "User: %s", cfg->username);
    print_centered(win, rows/2 + USER_ROW_OFFSET, cols, user_line, COLOR_PAIR(1));

    // Password prompt
    int prompt_col = cols * PROMPT_COL_FACTOR;
    mvwprintw(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col, "password: ");

    // Password input (asterisks)
    wattron(win, A_REVERSE);
    for (int i = 0; i < input_pos; i++) {
        mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + i, '*');
    }
    wattroff(win, A_REVERSE);

    // Error message
    if (error_msg[0]) {
        print_centered(win, rows/2 + ERROR_ROW_OFFSET, cols, error_msg, COLOR_PAIR(2));
    }

    // Move cursor
    wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + input_pos);
    wrefresh(win);
}

// Handle password input, return 1 if command should run, 2 if exit, 0 otherwise
int handle_input(WINDOW *win, int rows, int cols, Config *cfg, char *input, int *pos, char *error_msg) {
    int prompt_col = cols * PROMPT_COL_FACTOR + strlen("password: ");
    int ch = getch();

    if (ch == 27) { // ESC to exit
        return 2;
    }
    else if (ch == '\n') {
        if (!strcmp(input, cfg->pwd)) {
            return 1; // Correct password
        }
        strcpy(error_msg, "Incorrect password");
        input[0] = '\0';
        *pos = 0;
    }
    else if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && *pos > 0) {
        input[--(*pos)] = '\0';
        mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos, ' ');
    }
    else if (ch >= ' ' && ch <= '~' && *pos < MAX_PASSWORD_LEN) {
        input[(*pos)++] = ch;
        input[*pos] = '\0';
        wattron(win, A_REVERSE);
        mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos - 1, '*');
        wattroff(win, A_REVERSE);
    }

    // Move cursor and refresh
    wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos);
    wrefresh(win);

    return 0;
}

int main() {
    // Initialize config
    Config cfg = {
        .username = "guest",
        .pwd = "",
        .cmd = "true"
    };

    // Initialize NCurses
    initscr();
    clear();
    refresh(); // Ensure stdscr is drawn
    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors\n");
        return 1;
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK); // Title/username
    init_pair(2, COLOR_RED, COLOR_BLACK);   // Errors
    init_pair(3, COLOR_BLUE, COLOR_BLACK);  // Border
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(1); // Show cursor

    // Check terminal size
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        endwin();
        fprintf(stderr, "Terminal too small (min %dx%d)\n", MIN_COLS, MIN_ROWS);
        return 1;
    }

    // Create login window (80% of screen, centered)
    int win_rows = rows * 0.8;
    int win_cols = cols * 0.8;
    if (win_rows > rows - 2) win_rows = rows - 2;
    if (win_cols > cols - 2) win_cols = cols - 2;
    int win_y = (rows - win_rows) / 2;
    int win_x = (cols - win_cols) / 2;
    WINDOW *win = newwin(win_rows, win_cols, win_y, win_x);
    wbkgd(win, COLOR_PAIR(3)); // Blue border

    // Signal handling
    signal(SIGINT, handle_sigint);
    signal(SIGWINCH, handle_sigwinch);

    // Read .dmrc
    char error_msg[256] = "";
    if (read_dmrc(&cfg) != 0) {
        strcpy(error_msg, "No ~/.dmrc found, using defaults");
        draw_ui(win, win_rows, win_cols, &cfg, error_msg, 0);
        sleep(3);
        error_msg[0] = '\0';
    }

    // Validate config
    if (!cfg.username[0] || !cfg.cmd[0]) {
        strcpy(error_msg, "Invalid .dmrc: missing username or cmd");
        draw_ui(win, win_rows, win_cols, &cfg, error_msg, 0);
        sleep(3);
        strcpy(cfg.username, "guest");
        strcpy(cfg.cmd, "true");
        error_msg[0] = '\0';
    }

    char input[MAX_PASSWORD_LEN + 1] = "";
    int pos = 0;
    time_t error_start = 0;

    // Draw initial UI
    draw_ui(win, win_rows, win_cols, &cfg, error_msg, pos);
    usleep(100000); // 100ms delay for Termux stability

    while (running) {
        // Handle resize
        if (resized) {
            resized = 0;
            clear();
            refresh();
            getmaxyx(stdscr, rows, cols);
            if (rows < MIN_ROWS || cols < MIN_COLS) {
                endwin();
                fprintf(stderr, "Terminal too small (min %dx%d)\n", MIN_COLS, MIN_ROWS);
                return 1;
            }
            win_rows = rows * 0.8;
            win_cols = cols * 0.8;
            if (win_rows > rows - 2) win_rows = rows - 2;
            if (win_cols > cols - 2) win_cols = cols - 2;
            win_y = (rows - win_rows) / 2;
            win_x = (cols - win_cols) / 2;
            delwin(win);
            win = newwin(win_rows, win_cols, win_y, win_x);
            wbkgd(win, COLOR_PAIR(3));
            draw_ui(win, win_rows, win_cols, &cfg, error_msg, pos);
        }

        // Clear error after timeout
        if (error_msg[0] && time(NULL) - error_start >= ERROR_TIMEOUT) {
            error_msg[0] = '\0';
            draw_ui(win, win_rows, win_cols, &cfg, error_msg, pos);
        }

        int action = handle_input(win, win_rows, win_cols, &cfg, input, &pos, error_msg);
        if (action == 1) { // Run command
            wclear(win);
            wrefresh(win);
            delwin(win);
            endwin();
            int ret = system(cfg.cmd);
            if (ret != 0) {
                fprintf(stderr, "Command '%s' failed with code %d\n", cfg.cmd, ret);
                initscr();
                clear();
                refresh();
                noecho();
                cbreak();
                keypad(stdscr, TRUE);
                curs_set(1);
                getmaxyx(stdscr, rows, cols);
                win_rows = rows * 0.8;
                win_cols = cols * 0.8;
                if (win_rows > rows - 2) win_rows = rows - 2;
                if (win_cols > cols - 2) win_cols = cols - 2;
                win_y = (rows - win_rows) / 2;
                win_x = (cols - win_cols) / 2;
                win = newwin(win_rows, win_cols, win_y, win_x);
                wbkgd(win, COLOR_PAIR(3));
                strcpy(error_msg, "Command failed, retrying login");
                error_start = time(NULL);
                input[0] = '\0';
                pos = 0;
                draw_ui(win, win_rows, win_cols, &cfg, error_msg, pos);
                continue;
            }
            return 0;
        }
        else if (action == 2) { // Exit
            break;
        }

        if (error_msg[0] && !error_start) {
            error_start = time(NULL);
            draw_ui(win, win_rows, win_cols, &cfg, error_msg, pos);
        }
    }

    wclear(win);
    wrefresh(win);
    delwin(win);
    endwin();
    return 0;
}
