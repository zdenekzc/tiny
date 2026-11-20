/* Files, tabs, output lines and the expanded project tree. */
#include "tiny_ide.h"

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void set_status(struct app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
}

const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool path_join(char *out, size_t size, const char *base, const char *name)
{
    if (!out || !size || !base || !name)
        return false;
    if (name[0] == '/') {
        int n = snprintf(out, size, "%s", name);
        return n >= 0 && (size_t)n < size;
    }
    size_t length = strlen(base);
    int n = snprintf(out, size, "%s%s%s", base,
                     length && base[length - 1] == '/' ? "" : "/", name);
    return n >= 0 && (size_t)n < size;
}

bool make_absolute(char *out, size_t size, const char *base, const char *path)
{
    if (!path || !path[0]) {
        errno = ENOENT;
        return false;
    }
    if (!path_join(out, size, base, path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool make_existing_absolute(char *out, size_t size, const char *base,
                            const char *path)
{
    char temporary[PATH_MAX], resolved[PATH_MAX];
    if (!make_absolute(temporary, sizeof(temporary), base, path))
        return false;
    if (!realpath(temporary, resolved))
        return false;
    if (strlen(resolved) >= size) {
        errno = ENAMETOOLONG;
        return false;
    }
    snprintf(out, size, "%s", resolved);
    return true;
}

bool has_extension(const char *name, const char *extension)
{
    size_t n = strlen(name), e = strlen(extension);
    if (n < e)
        return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n - e + i]) !=
            tolower((unsigned char)extension[i]))
            return false;
    return true;
}

bool is_cpp_file(const char *name)
{
    return has_extension(name, ".cc") || has_extension(name, ".cpp") ||
           has_extension(name, ".cxx");
}

char *read_whole_file(const char *path, size_t *length)
{
    *length = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uintmax_t)st.st_size > EDITOR_MAX_FILE) {
        int error = errno;
        if (error == 0 || (uintmax_t)st.st_size > EDITOR_MAX_FILE)
            error = EFBIG;
        close(fd);
        errno = error;
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size + 1);
    if (!data) {
        close(fd);
        return NULL;
    }
    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, data + done, size - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            int error = got < 0 ? errno : EIO;
            free(data);
            close(fd);
            errno = error;
            return NULL;
        }
        done += (size_t)got;
    }
    close(fd);
    data[size] = '\0';
    if (memchr(data, '\0', size)) {
        free(data);
        errno = EINVAL;
        return NULL;
    }
    *length = size;
    return data;
}

bool write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (written == 0)
                errno = EIO;
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

/* Write beside the target and rename only after a complete write.  For an
 * existing file realpath() deliberately follows symlinks, preserving them. */
bool write_file_atomic(const char *path, const char *data, size_t length)
{
    char *target = realpath(path, NULL);
    bool existed = target != NULL;
    if (!target) {
        target = strdup(path);
        if (!target)
            return false;
    }

    struct stat original;
    bool have_original = existed && stat(target, &original) == 0;
    size_t target_length = strlen(target);
    if (target_length > SIZE_MAX - 11) {
        free(target);
        errno = ENAMETOOLONG;
        return false;
    }
    char *temporary = malloc(target_length + 11);
    if (!temporary) {
        free(target);
        return false;
    }
    snprintf(temporary, target_length + 11, "%s.tmpXXXXXX", target);
    int fd = mkstemp(temporary);
    if (fd < 0) {
        free(temporary);
        free(target);
        return false;
    }

    int error = 0;
    if (!write_all(fd, data, length))
        error = errno;
    if (!error && have_original &&
        (original.st_uid != geteuid() || original.st_gid != getegid()) &&
        fchown(fd, original.st_uid, original.st_gid) != 0) {
        /* Only root may change ownership; as an unprivileged user the saved
         * file keeps the caller's identity instead (a ToaruOS guest runs
         * as "local", the Linux build as root).  Any other fchown error
         * still fails the save. */
        if (errno != EPERM)
            error = errno;
    }
    if (!error && fchmod(fd, have_original ? original.st_mode & 07777 : 0644) != 0)
        error = errno;
    if (!error && fsync(fd) != 0)
        error = errno;
    if (close(fd) != 0 && !error)
        error = errno;
    if (!error && rename(temporary, target) != 0)
        error = errno;
    if (error)
        unlink(temporary);
    free(temporary);
    free(target);
    if (error) {
        errno = error;
        return false;
    }
    return true;
}

static void tab_init(struct tab *tab, const char *path, const char *data,
                     size_t length)
{
    memset(tab, 0, sizeof(*tab));
    editor_init(&tab->ed);
    if (editor_set_text(&tab->ed, data, length) != 0)
        return;
    tab->untitled = !path || !path[0];
    if (!tab->untitled)
        snprintf(tab->path, sizeof(tab->path), "%s", path);
    snprintf(tab->title, sizeof(tab->title), "%s",
             tab->untitled ? "untitled.c" : base_name(path));
}

int find_open_tab(struct app *app, const char *path)
{
    for (int i = 0; i < app->tab_count; ++i)
        if (!app->tabs[i].untitled && strcmp(app->tabs[i].path, path) == 0)
            return i;
    return -1;
}

bool new_tab(struct app *app)
{
    if (app->tab_count >= MAX_TABS) {
        set_status(app, "Too many tabs (maximum %d)", MAX_TABS);
        return false;
    }
    int index = app->tab_count;
    tab_init(&app->tabs[index], "", "", 0);
    if (!app->tabs[index].ed.text) {
        set_status(app, "Cannot create a new editor buffer: %s", strerror(errno));
        return false;
    }
    app->tab_count++;
    app->active = index;
    app->focus = FOCUS_EDITOR;
    set_status(app, "New file - F3 or Ctrl+S chooses its path");
    return true;
}

bool open_file(struct app *app, const char *path)
{
    char absolute[PATH_MAX];
    if (!make_existing_absolute(absolute, sizeof(absolute), app->project, path)) {
        set_status(app, "Cannot open %s: %s", path, strerror(errno));
        return false;
    }
    int existing = find_open_tab(app, absolute);
    if (existing >= 0) {
        app->active = existing;
        app->focus = FOCUS_EDITOR;
        set_status(app, "Switched to %s", base_name(absolute));
        return true;
    }
    if (app->tab_count >= MAX_TABS) {
        set_status(app, "Too many tabs (maximum %d)", MAX_TABS);
        return false;
    }
    size_t length = 0;
    char *data = read_whole_file(absolute, &length);
    if (!data) {
        set_status(app, "Cannot read %s: %s", absolute,
                   errno == EINVAL ? "binary file" : strerror(errno));
        return false;
    }
    tab_init(&app->tabs[app->tab_count], absolute, data, length);
    free(data);
    if (!app->tabs[app->tab_count].ed.text) {
        set_status(app, "Cannot load %s: %s", absolute, strerror(errno));
        return false;
    }
    app->active = app->tab_count++;
    app->focus = FOCUS_EDITOR;
    set_status(app, "Opened %s", absolute);
    return true;
}

bool save_tab(struct app *app, int index, const char *path)
{
    if (index < 0 || index >= app->tab_count)
        return false;
    char absolute[PATH_MAX];
    if (!make_absolute(absolute, sizeof(absolute), app->project, path)) {
        set_status(app, "Invalid save path: %s", strerror(errno));
        return false;
    }
    struct tab *tab = &app->tabs[index];
    if (!write_file_atomic(absolute, tab->ed.text, tab->ed.length)) {
        set_status(app, "Save failed: %s", strerror(errno));
        return false;
    }
    snprintf(tab->path, sizeof(tab->path), "%s", absolute);
    snprintf(tab->title, sizeof(tab->title), "%.255s", base_name(absolute));
    tab->untitled = false;
    editor_mark_saved(&tab->ed);
    set_status(app, "Saved %s", absolute);
    tree_rebuild(app);
    return true;
}

void close_tab_index(struct app *app, int index)
{
    if (index < 0 || index >= app->tab_count)
        return;
    char title[256];
    snprintf(title, sizeof(title), "%s", app->tabs[index].title);
    editor_free(&app->tabs[index].ed);
    for (int i = index; i + 1 < app->tab_count; ++i)
        app->tabs[i] = app->tabs[i + 1];
    app->tab_count--;
    memset(&app->tabs[app->tab_count], 0, sizeof(app->tabs[app->tab_count]));
    if (!app->tab_count) {
        app->active = -1;
        app->focus = FOCUS_EXPLORER;
    } else if (app->active > index) {
        app->active--;
    } else if (app->active == index) {
        app->active = index < app->tab_count ? index : app->tab_count - 1;
    }
    app->dragging = false;
    set_status(app, "Closed %s", title);
}

bool any_dirty_tab(struct app *app)
{
    for (int i = 0; i < app->tab_count; ++i)
        if (app->tabs[i].ed.revision != app->tabs[i].ed.saved_revision)
            return true;
    return false;
}

/* --------------------------------------------------------------- output */

void output_clear(struct app *app)
{
    output_free(app);
    app->output_follow = true;
    app->output_selected = 0;
    app->output_scroll = 0;
    app->output_left = 0;
}

static struct output_line *output_new_line(struct app *app)
{
    struct output_line *line = calloc(1, sizeof(*line));
    if (!line)
        return NULL;
    line->capacity = 128;
    line->text = malloc(line->capacity);
    if (!line->text) {
        free(line);
        return NULL;
    }
    line->text[0] = '\0';
    if (app->output_tail)
        app->output_tail->next = line;
    else
        app->output_head = line;
    app->output_tail = line;
    app->output_count++;
    if (app->output_count > OUTPUT_MAX_LINES && app->output_head != line) {
        struct output_line *old = app->output_head;
        app->output_head = old->next;
        free(old->text);
        free(old);
        app->output_count--;
        if (app->output_selected > 0)
            app->output_selected--;
        if (app->output_scroll > 0)
            app->output_scroll--;
    }
    if (app->output_follow)
        app->output_selected = app->output_count - 1;
    return line;
}

void output_append(struct app *app, const char *text, size_t length)
{
    while (length) {
        if (!app->output_tail || app->output_tail->complete)
            if (!output_new_line(app))
                return;
        struct output_line *line = app->output_tail;
        const char *newline = memchr(text, '\n', length);
        size_t amount = newline ? (size_t)(newline - text) : length;
        size_t room = OUTPUT_LINE_MAX - 1 - line->length;
        if (amount > room)
            amount = room;
        if (amount) {
            if (line->length + amount + 1 > line->capacity) {
                size_t capacity = line->capacity;
                while (capacity < line->length + amount + 1)
                    capacity *= 2;
                char *grown = realloc(line->text, capacity);
                if (!grown)
                    return;
                line->text = grown;
                line->capacity = capacity;
            }
            memcpy(line->text + line->length, text, amount);
            line->length += amount;
            line->text[line->length] = '\0';
            text += amount;
            length -= amount;
        }
        if (!length)
            break;
        /* A full physical line is still terminated at the real newline. */
        if (*text == '\n') {
            if (line->length && line->text[line->length - 1] == '\r')
                line->text[--line->length] = '\0';
            line->complete = true;
            text++;
            length--;
        } else {
            size_t skip = strcspn(text, "\n");
            text += skip;
            length -= skip;
        }
    }
}

void output_printf(struct app *app, const char *fmt, ...)
{
    char buffer[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (n > 0)
        output_append(app, buffer, (size_t)n < sizeof(buffer) ?
                      (size_t)n : sizeof(buffer) - 1);
}

void output_flush(struct app *app)
{
    if (app->output_tail && app->output_tail->length)
        app->output_tail->complete = true;
}

void output_free(struct app *app)
{
    struct output_line *line = app->output_head;
    while (line) {
        struct output_line *next = line->next;
        free(line->text);
        free(line);
        line = next;
    }
    app->output_head = app->output_tail = NULL;
    app->output_count = 0;
}

struct output_line *output_line_at(struct app *app, int index)
{
    if (index < 0)
        return NULL;
    struct output_line *line = app->output_head;
    while (line && index--)
        line = line->next;
    return line;
}

/* ----------------------------------------------------------- file tree */

struct file_item {
    char *name;
    bool is_dir;
};

static int compare_file_items(const void *a, const void *b)
{
    const struct file_item *left = a, *right = b;
    if (left->is_dir != right->is_dir)
        return left->is_dir ? -1 : 1;
    return strcmp(left->name, right->name);
}

static bool skipped_directory(const char *name)
{
    static const char *const skipped[] = {
        ".git", ".hg", ".svn", "node_modules", "__pycache__", "build",
        "target", "dist", "obj", "_build", "_output", "_tmp", NULL
    };
    for (int i = 0; skipped[i]; ++i)
        if (strcmp(name, skipped[i]) == 0)
            return true;
    return false;
}

static int expanded_index(struct app *app, const char *path)
{
    for (int i = 0; i < app->expanded_count; ++i)
        if (strcmp(app->expanded[i], path) == 0)
            return i;
    return -1;
}

static bool tree_expand(struct app *app, const char *path)
{
    if (expanded_index(app, path) >= 0)
        return true;
    if (app->expanded_count == app->expanded_capacity) {
        int capacity = app->expanded_capacity ? app->expanded_capacity * 2 : 16;
        char **grown = realloc(app->expanded, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        app->expanded = grown;
        app->expanded_capacity = capacity;
    }
    char *copy = strdup(path);
    if (!copy)
        return false;
    app->expanded[app->expanded_count++] = copy;
    return true;
}

static void tree_collapse(struct app *app, const char *path)
{
    size_t length = strlen(path);
    int out = 0;
    for (int i = 0; i < app->expanded_count; ++i) {
        bool inside = strncmp(app->expanded[i], path, length) == 0 &&
                      (app->expanded[i][length] == '\0' ||
                       app->expanded[i][length] == '/');
        if (inside) {
            free(app->expanded[i]);
        } else {
            app->expanded[out++] = app->expanded[i];
        }
    }
    app->expanded_count = out;
}

static bool push_tree_item(struct app *app, const char *path, const char *name,
                           int depth, bool is_dir)
{
    if (app->tree_count >= MAX_TREE_ITEMS)
        return false;
    struct tree_item *item = &app->tree[app->tree_count];
    if (snprintf(item->path, sizeof(item->path), "%s", path) >=
            (int)sizeof(item->path) ||
        snprintf(item->name, sizeof(item->name), "%s", name) >=
            (int)sizeof(item->name))
        return false;
    item->depth = depth;
    item->is_dir = is_dir;
    item->expanded = is_dir && expanded_index(app, path) >= 0;
    app->tree_count++;
    return true;
}

static void tree_walk(struct app *app, const char *path, int depth)
{
    if (depth > MAX_TREE_DEPTH || app->tree_count >= MAX_TREE_ITEMS)
        return;
    DIR *directory = opendir(path);
    if (!directory)
        return;
    struct file_item *items = NULL;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] == '.' || skipped_directory(name))
            continue;
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), path, name))
            continue;
        struct stat st;
        if (lstat(full, &st) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
            continue;
        struct file_item *grown = realloc(items, (size_t)(count + 1) * sizeof(*items));
        if (!grown)
            break;
        items = grown;
        items[count].name = strdup(name);
        if (!items[count].name)
            break;
        items[count].is_dir = S_ISDIR(st.st_mode);
        count++;
    }
    closedir(directory);
    qsort(items, (size_t)count, sizeof(*items), compare_file_items);
    for (int i = 0; i < count && app->tree_count < MAX_TREE_ITEMS; ++i) {
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), path, items[i].name))
            continue;
        if (!push_tree_item(app, full, items[i].name, depth, items[i].is_dir))
            continue;
        if (items[i].is_dir && expanded_index(app, full) >= 0)
            tree_walk(app, full, depth + 1);
    }
    for (int i = 0; i < count; ++i)
        free(items[i].name);
    free(items);
}

void tree_rebuild(struct app *app)
{
    char selected_path[PATH_MAX] = "";
    if (app->tree_selected >= 0 && app->tree_selected < app->tree_count)
        snprintf(selected_path, sizeof(selected_path), "%s",
                 app->tree[app->tree_selected].path);
    app->tree_count = 0;
    tree_walk(app, app->project, 0);
    if (selected_path[0]) {
        for (int i = 0; i < app->tree_count; ++i)
            if (strcmp(app->tree[i].path, selected_path) == 0) {
                app->tree_selected = i;
                break;
            }
    }
    if (!app->tree_count)
        app->tree_selected = 0;
    else if (app->tree_selected >= app->tree_count)
        app->tree_selected = app->tree_count - 1;
    if (app->tree_selected < 0)
        app->tree_selected = 0;
}

void tree_toggle(struct app *app, int index)
{
    if (index < 0 || index >= app->tree_count || !app->tree[index].is_dir)
        return;
    app->tree_selected = index;
    if (expanded_index(app, app->tree[index].path) >= 0)
        tree_collapse(app, app->tree[index].path);
    else if (!tree_expand(app, app->tree[index].path)) {
        set_status(app, "Out of memory while expanding directory");
        return;
    }
    tree_rebuild(app);
}

void tree_free(struct app *app)
{
    free(app->tree);
    app->tree = NULL;
    for (int i = 0; i < app->expanded_count; ++i)
        free(app->expanded[i]);
    free(app->expanded);
    app->expanded = NULL;
    app->expanded_count = app->expanded_capacity = 0;
}
