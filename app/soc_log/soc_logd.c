#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>
#include <libgen.h>     // for process file path
#include <limits.h>     // for PATH_MAX
#include <dirent.h>     // for path ops
#include <ctype.h>      // for isspace()
#include <signal.h>     // for signal
#include "quard_soc_log.h"
#include "../../common_inc/bsp/quard_log.h"

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_ERROR 2

#define DISABLE_FSI_R52_LOG

#ifdef VARIANT_USER
    #define LOG_LEVEL LOG_LEVEL_ERROR
#elif defined(VARIANT_USERDEBUG)
    #define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

#if LOG_LEVEL >= LOG_LEVEL_ERROR
    #define LOG_PRINTF(...)
#else
    #define LOG_PRINTF(...) printf(__VA_ARGS__)
#endif
#define LOG_PERROR(msg) perror(msg)

#define IOCTL_SET_BUFFER _IOW('a', 'a', int32_t*)
#define DEVICE_PATH                     "/dev/log_device"

#define CONFIG_FILE_PATH                "/etc/soc_logd.conf"
#define DEFAULT_MAX_SIZE                (100 * 1024 * 1024)  // default 10MB
#define DEFAULT_MAX_FILES               5                   // default 5 backup file

#define NETLINK_SOC_LOG         31
#define MAX_PAYLOAD             1024
#define MEMORY_CONTENT          "This is the memory content to be written to file.\n"
#define DEFAULT_OUTPUT_DIR      "/opt/soc_log"
#define MCU_LOG_FILE            "mcu_log.log"
#define LINUX_BOOT_LOG_FILE     "linux_boot_log.log"

typedef enum conf_index {
	CONF_IDX_MCU = 0,
	CONF_IDX_LINUX,
	CONF_IDX_MAX,
} conf_enum;

typedef enum fw_index {
    FW_IDX_MCU = 0,
    FW_IDX_BL1_SBI,
    FW_IDX_UBOOT,
    FW_IDX_MAX,
} fw_enum;

#define QUARD_RINGBUFFER_EMPTY             (0)
#define QUARD_RINGBUFFER_FULL              (1)
#define QUARD_RINGBUFFER_HALFFULL          (2)

// log file config
typedef struct {
    char name[64];      // log file name
    size_t max_size;    // log file max size
    int max_files;      // log file max num
} log_config_t;

// global config
typedef struct {
    log_config_t configs[CONF_IDX_MAX];  // core num
    int config_count;
    char output_dir[PATH_MAX]; // log path
} global_config_t;

// global config
global_config_t g_config;

volatile sig_atomic_t running = 1;
int g_fd = -1;                              
int g_sock_fd = -1;                         
void *g_mapped_bufs[FW_IDX_MAX] = {NULL};   
size_t g_buf_sizes[FW_IDX_MAX] = {0};       

void dump_memory(void *addr, size_t length)
{
    unsigned char *ptr = (unsigned char *)addr;
    for (size_t i = 0; i < length; i++) {
            if (i % 16 == 0) {
                    LOG_PRINTF("\n%08lx: ", (unsigned long)(ptr + i));
            }
            LOG_PRINTF("%02x ", ptr[i]);
    }

    LOG_PRINTF("\n");
}

static void create_directories(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; ++p) {
    if (*p == '/') {
        *p = '\0';
        if (mkdir(tmp, 0755) && errno != EEXIST) {
                LOG_PERROR("Failed to create directory");
                exit(EXIT_FAILURE);
        }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) && errno != EEXIST) {
        LOG_PERROR("Failed to create directory");
        exit(EXIT_FAILURE);
    }
}

static int quard_ringbuffer_status(struct quard_log_ringbuffer *rb)
{
    if (rb->read_index == rb->write_index)
    {
        if (rb->read_mirror == rb->write_mirror)
            return QUARD_RINGBUFFER_EMPTY;
        else
            return QUARD_RINGBUFFER_FULL;
    }

    return QUARD_RINGBUFFER_HALFFULL;
}

uint32_t quard_ringbuffer_data_len(struct quard_log_ringbuffer *rb)
{
    switch (quard_ringbuffer_status(rb))
    {
    case QUARD_RINGBUFFER_EMPTY:
        LOG_PRINTF("ringbuffer is empty\n");
        return 0;
    case QUARD_RINGBUFFER_FULL:
        return rb->buffer_size;
    case QUARD_RINGBUFFER_HALFFULL:
    default:
    {
        uint32_t wi = rb->write_index, ri = rb->read_index;

        if (wi > ri)
            return wi - ri;
        else
            return rb->buffer_size - (ri - wi);
    }
    }
}

// gen timestamp
void generate_timestamp(char *timestamp, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(timestamp, size, "%Y%m%d-%H%M%S", tm_info);
}

// get file size
size_t get_file_size(const char *filename)
{
    struct stat st;

    if (stat(filename, &st) == 0) {
        return st.st_size;
    }

    return 0;
}

int compare_filenames(const void *a, const void *b)
{
    const char *fa = *(const char **)a;
    const char *fb = *(const char **)b;

    const char *ta = strrchr(fa, '.');
    const char *tb = strrchr(fb, '.');

    if (ta && tb) {
        return strcmp(tb, ta);
    }

    return strcmp(fa, fb);
}

// clean old log
void cleanup_old_logs(const char *base_filename, int max_files)
{
    char dir_path[PATH_MAX];
    char base_name[PATH_MAX];
    char *filename_copy = strdup(base_filename);

    if (!filename_copy) {
        LOG_PERROR("strdup failed");
        return;
    }

    // get dir
    char *dir = dirname(filename_copy);
    strcpy(dir_path, dir);
    free(filename_copy);

    // get file name
    filename_copy = strdup(base_filename);
    if (!filename_copy) {
        LOG_PERROR("strdup failed");
        return;
    }
    char *base = basename(filename_copy);
    strcpy(base_name, base);
    free(filename_copy);

    // open dir
    DIR *dir_handle = opendir(dir_path);
    if (!dir_handle) {
        LOG_PERROR("opendir failed");
        return;
    }

    struct dirent *entry;
    char **backup_files = NULL;
    int file_count = 0;
    size_t base_len = strlen(base_name);

    // iterate the dir
    while ((entry = readdir(dir_handle)) != NULL) {
        if (strncmp(entry->d_name, base_name, base_len) == 0 && 
            entry->d_name[base_len] == '.' && 
            isdigit(entry->d_name[base_len + 1])) {

            backup_files = realloc(backup_files, (file_count + 1) * sizeof(char *));
            if (!backup_files) {
                LOG_PERROR("realloc failed");
                closedir(dir_handle);
                return;
            }

            backup_files[file_count] = strdup(entry->d_name);
            if (!backup_files[file_count]) {
                LOG_PERROR("strdup failed");
                closedir(dir_handle);
                return;
            }

            file_count++;
        }
    }

    closedir(dir_handle);

    if (file_count > max_files) {
        // backup_files 
        qsort(backup_files, file_count, sizeof(char *), compare_filenames);

        for (int i = max_files; i < file_count; i++) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, backup_files[i]);

            if (unlink(full_path) != 0) {   
                LOG_PERROR("Failed to delete old log file");
            } else {
                LOG_PRINTF("Deleted old log file: %s\n", full_path);
            }
        }
    }

    for (int i = 0; i < file_count; i++) {
        free(backup_files[i]);    
    }
    free(backup_files);
}

void rotate_log(const char *filename, const log_config_t *config)
{
    char timestamp[32];
    char backup_name[PATH_MAX];
    char file_path[PATH_MAX];

    generate_timestamp(timestamp, sizeof(timestamp));

    snprintf(file_path, sizeof(file_path), "%s/%s", g_config.output_dir, filename);

    snprintf(backup_name, sizeof(backup_name), "%s.%s", file_path, timestamp);

    if (rename(file_path, backup_name) != 0) {
        LOG_PRINTF("file_path: %s\n", file_path);
        LOG_PERROR("Failed to rename log file");
        return;
    }

    LOG_PRINTF("Rotated log file: %s -> %s\n", file_path, backup_name);

    cleanup_old_logs(file_path, config->max_files);
}

const log_config_t* find_config_for_core(const char *filename)
{
    for (int i = 0; i < g_config.config_count; i++) {
        if (strstr(filename, g_config.configs[i].name) != NULL) {
            return &g_config.configs[i];
        }
    }

    if (g_config.config_count > 0) {
        return &g_config.configs[0];
    }

    return NULL;
}

void write_to_file(const char *filename, const char *data, int len)
{
    static int dir_checked = 0;
    uint32_t valid_size  = 0;
    char *payload_ptr;
    slave_buffer_cb *buf_ptr = (slave_buffer_cb *)data;
    struct quard_log_ringbuffer *rb = &buf_ptr->rb_ctrl;
    char file_path[PATH_MAX];

    if (!dir_checked) {
        create_directories(g_config.output_dir);
        dir_checked = 1;
    }

    if((buf_ptr->head_magic != MAGIC_PATTERN) ||
            (buf_ptr->middle_magic != MAGIC_PATTERN)) {
            LOG_PRINTF("[error]: magic pattern mismatch(expect:%x act_head:%x act_mid:%x\n)", 
            MAGIC_PATTERN, buf_ptr->head_magic, buf_ptr->middle_magic);
            // dump_memory(buf_ptr, 256);
            return;
    }

    valid_size  = quard_ringbuffer_data_len(rb);
    LOG_PRINTF("buffer size: 0x%08x\n", valid_size );
    payload_ptr = (char *)(&buf_ptr->buffer_first_byte);

    snprintf(file_path, sizeof(file_path), "%s/%s", g_config.output_dir, filename);

    const log_config_t *config = find_config_for_core(filename);

    if (config != NULL) {
        size_t current_size = get_file_size(file_path);
        if (current_size + valid_size > config->max_size) {
            rotate_log(filename, config);
        }
    }

    int fd = open(file_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd == -1) {
            LOG_PERROR("open");
            exit(EXIT_FAILURE);
    }

    if (len == 0)
        write(fd, data, strlen(data));
    else {
        uint32_t first_part_size = rb->buffer_size - rb->read_index;
        if (first_part_size > valid_size) {
            first_part_size = valid_size;
        }

        if (write(fd, payload_ptr + rb->read_index, first_part_size) < 0) {
            LOG_PERROR("Failed to write first part to file");
            close(fd);
            return;
        }

        if (valid_size > first_part_size) {
            uint32_t second_part_size = valid_size - first_part_size;
            if (write(fd, payload_ptr, second_part_size) < 0) {
                    LOG_PERROR("Failed to write second part to file");
                    close(fd);
                    return;
            }
        }
    }

    close(fd);
    // dump_memory((void *)data, 256);
}

static int write_all(int fd, const void *data, size_t len)
{
    const char *ptr = data;

    while (len > 0) {
        ssize_t written = write(fd, ptr, len);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        ptr += written;
        len -= (size_t)written;
    }

    return 0;
}

static int ramlog_valid_len(const void *region, size_t region_size,
                            uint32_t *head, uint32_t *size,
                            uint32_t *valid_len)
{
    const struct ramlog_buffer *rb = region;

    if (region == NULL || region_size <= sizeof(*rb) ||
        rb->magic != RAMLOG_MAGIC ||
        rb->size != region_size - sizeof(*rb) || rb->size <= 1 ||
        rb->head >= rb->size || rb->tail >= rb->size) {
        return -1;
    }

    *head = rb->head;
    *size = rb->size;
    *valid_len = rb->tail >= rb->head ? rb->tail - rb->head :
                 rb->size - rb->head + rb->tail;
    return 0;
}

static int append_ramlog_to_file(int fd, const char *stage,
                                 const void *region, size_t region_size)
{
    const struct ramlog_buffer *rb = region;
    const char *payload = (const char *)(rb + 1);
    char separator[64];
    uint32_t head, size, valid_len, first_len;
    int separator_len;

    if (ramlog_valid_len(region, region_size, &head, &size, &valid_len) < 0) {
        fprintf(stderr, "Invalid %s ramlog metadata, skipping stage\n", stage);
        return -1;
    }

    separator_len = snprintf(separator, sizeof(separator),
                             "===== %s =====\n", stage);
    if (separator_len < 0 || (size_t)separator_len >= sizeof(separator) ||
        write_all(fd, separator, (size_t)separator_len) < 0) {
        LOG_PERROR("write ramlog separator");
        return -1;
    }

    first_len = size - head;
    if (first_len > valid_len) {
        first_len = valid_len;
    }
    if (write_all(fd, payload + head, first_len) < 0 ||
        (valid_len > first_len &&
         write_all(fd, payload, valid_len - first_len) < 0)) {
        LOG_PERROR("write ramlog payload");
        return -1;
    }

    return separator_len + valid_len;
}

int netlink_recv_message(int sock_fd, char *message, int *len)
{
        if(message == NULL|| len == NULL) {
                return -1;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
        if( !nlh ) {
                LOG_PERROR("malloc");
                return -1;
        }

        struct sockaddr_nl src_addr;
        socklen_t addrlen = sizeof(struct sockaddr_nl);
        memset(&src_addr, 0, addrlen);

        if(recvfrom(sock_fd, nlh, NLMSG_SPACE(MAX_PAYLOAD), 0, (struct sockaddr *)&src_addr, (socklen_t *)&addrlen) < 0 ) {
            LOG_PRINTF("recvmsg error!\n");
            free(nlh);
            return -1;
        }

        *len = nlh->nlmsg_len - NLMSG_SPACE(0);
        memcpy(message, (unsigned char *)NLMSG_DATA(nlh), *len);

        free(nlh);

        return 0;
}

void *access_buffer(int fd, int buffer_selection, int buf_size)
{
    void *mapped_mem;
    
    if (ioctl(fd, IOCTL_SET_BUFFER, &buffer_selection) == -1) {
        LOG_PERROR("Error setting buffer selection");
        return MAP_FAILED;
    }

    mapped_mem = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_mem == MAP_FAILED) {
        LOG_PERROR("Error mmapping the file");
        return MAP_FAILED;
    }

    return mapped_mem;
}

int load_config(const char *config_path, global_config_t *config)
{
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOG_PRINTF("Warning: Cannot open config file %s, using defaults\n", config_path);
        return -1;
    }

    char line[256];
    char current_section[64] = {0};
    int current_config_index = -1;
    int is_global_section = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';

        if (line[0] == '\0' || line[0] == '#') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, line + 1, sizeof(current_section) - 1);

                if (strcmp(current_section, "global") == 0) {
                    is_global_section = 1;
                    current_config_index = -1;
                } else {
                    is_global_section = 0;

                    current_config_index = -1;
                    for (int i = 0; i < config->config_count; i++) {
                        if (strcmp(config->configs[i].name, current_section) == 0) {
                            current_config_index = i;
                            break;
                        }
                    }

                    if (current_config_index == -1 && config->config_count < 6) {
                        current_config_index = config->config_count++;
                        strncpy(config->configs[current_config_index].name, current_section, sizeof(config->configs[0].name) - 1);
                        config->configs[current_config_index].max_size = DEFAULT_MAX_SIZE;
                        config->configs[current_config_index].max_files = DEFAULT_MAX_FILES;
                    }
                }
            }
        }
        else if (current_config_index != -1 || is_global_section) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = line;
                char *value = eq + 1;

                while (*key && isspace(*key)) key++;
                char *end = key + strlen(key) - 1;
                while (end > key && isspace(*end)) *end-- = '\0';

                while (*value && isspace(*value)) value++;
                end = value + strlen(value) - 1;
                while (end > value && isspace(*end)) *end-- = '\0';

                if (is_global_section) {
                    if (strcmp(key, "output_dir") == 0) {
                        strncpy(config->output_dir, value, sizeof(config->output_dir) - 1);
                    }
                } else {
                    if (strcmp(key, "max_size") == 0) {
                        config->configs[current_config_index].max_size = atoll(value);
                    } else if (strcmp(key, "max_files") == 0) {
                        config->configs[current_config_index].max_files = atoi(value);
                    }
                }
            }
        }
    }

    fclose(fp);
    return 0;
}

void init_default_config(global_config_t *config)
{
    config->config_count = CONF_IDX_MAX;

    // default output dir
    strncpy(config->output_dir, DEFAULT_OUTPUT_DIR, sizeof(config->output_dir) - 1);

    // mcu
    strncpy(config->configs[CONF_IDX_MCU].name, "mcu", sizeof(config->configs[CONF_IDX_MCU].name) - 1);
    config->configs[CONF_IDX_MCU].max_size = DEFAULT_MAX_SIZE;
    config->configs[CONF_IDX_MCU].max_files = DEFAULT_MAX_FILES;

    // linux
    strncpy(config->configs[CONF_IDX_LINUX].name, "linux", sizeof(config->configs[CONF_IDX_LINUX].name) - 1);
    config->configs[CONF_IDX_LINUX].max_size = DEFAULT_MAX_SIZE;
    config->configs[CONF_IDX_LINUX].max_files = DEFAULT_MAX_FILES;
}

void signal_handler(int signum)
{
    if (signum == SIGTERM || signum == SIGINT) {
        LOG_PRINTF("soc log daemon SIGTERM received, start to cleanup resource\n");
        running = 0;
    }
}

void cleanup_resources()
{
    for (int i = 0; i < FW_IDX_MAX; i++) {
        if (g_mapped_bufs[i] != NULL && g_mapped_bufs[i] != MAP_FAILED) {
            if (munmap(g_mapped_bufs[i], g_buf_sizes[i]) == -1) {
                LOG_PERROR("munmap");
            }
            g_mapped_bufs[i] = NULL;
        }
    }

    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }

    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }

    LOG_PRINTF("soc log daemon cleanup resource done\n");
}

void write_linux_boot_log(char *bl1_sbi_log_buf, char *uboot_log_buf)
{
    static int dir_checked;
    const struct {
        const char *name;
        const void *region;
        size_t region_size;
    } stages[] = {
        { "SPL", bl1_sbi_log_buf + SPL_LOG_BUF_OFFSET, SPL_LOG_BUF_SIZE },
        { "OpenSBI", bl1_sbi_log_buf + SBI_LOG_BUF_OFFSET, SBI_LOG_BUF_SIZE },
        { "U-Boot", uboot_log_buf, UBOOT_LOG_BUF_SIZE },
    };
    char file_path[PATH_MAX];
    size_t append_size = 0;
    int fd;

    if (!dir_checked) {
        create_directories(g_config.output_dir);
        dir_checked = 1;
    }

    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        uint32_t head, size, valid_len;

        if (ramlog_valid_len(stages[i].region, stages[i].region_size,
                             &head, &size, &valid_len) == 0) {
            append_size += strlen(stages[i].name) +
                           sizeof("=====  =====\n") - 1 + valid_len;
        }
    }

    snprintf(file_path, sizeof(file_path), "%s/%s",
             g_config.output_dir, LINUX_BOOT_LOG_FILE);
    const log_config_t *config = find_config_for_core(LINUX_BOOT_LOG_FILE);
    if (config != NULL && get_file_size(file_path) + append_size > config->max_size) {
        rotate_log(LINUX_BOOT_LOG_FILE, config);
    }

    fd = open(file_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        LOG_PERROR("open linux boot log");
        goto unmap;
    }

    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        (void)append_ramlog_to_file(fd, stages[i].name, stages[i].region,
                                    stages[i].region_size);
    }
    close(fd);

    // dump_memory(bl1_sbi_log_buf + 0x1400, 256);
unmap:
    if (g_mapped_bufs[FW_IDX_BL1_SBI] != NULL && g_mapped_bufs[FW_IDX_BL1_SBI] != MAP_FAILED) {
        if (munmap(g_mapped_bufs[FW_IDX_BL1_SBI], g_buf_sizes[FW_IDX_BL1_SBI]) == -1) {
            LOG_PERROR("munmap");
        }
        g_mapped_bufs[FW_IDX_BL1_SBI] = NULL;
    }

    if (g_mapped_bufs[FW_IDX_UBOOT] != NULL && g_mapped_bufs[FW_IDX_UBOOT] != MAP_FAILED) {
        if (munmap(g_mapped_bufs[FW_IDX_UBOOT], g_buf_sizes[FW_IDX_UBOOT]) == -1) {
            LOG_PERROR("munmap");
        }
        g_mapped_bufs[FW_IDX_UBOOT] = NULL;
    }
}

int main(void)
{
    char *mcu_log_buf;
    char *uboot_log_buf, *bl1_sbi_log_buf;

    /* init default log config */
    init_default_config(&g_config);

    // try to load config file
    if (load_config(CONFIG_FILE_PATH, &g_config) == 0) {
        LOG_PRINTF("Loaded configuration from %s\n", CONFIG_FILE_PATH);
    } else {
        LOG_PRINTF("Using default configuration\n");
    }

    // current configs
    LOG_PRINTF("Current configuration:\n");
    LOG_PRINTF("  Output directory: %s\n", g_config.output_dir);
    for (int i = 0; i < g_config.config_count; i++) {
        LOG_PRINTF("  %s: max_size=%zu bytes, max_files=%d\n",
               g_config.configs[i].name,
               g_config.configs[i].max_size,
               g_config.configs[i].max_files);
    }

    // regsiter sig handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // init global data
    for (int i = 0; i < FW_IDX_MAX; i++) {
        g_mapped_bufs[i] = NULL;
        g_buf_sizes[i] = 0;
    }

    struct sockaddr_nl src_addr, dst_addr;
    struct nlmsghdr *nlh;
    char recv_buf[MAX_PAYLOAD] = {0};
    int recv_len = 0;

    g_sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_SOC_LOG);
    if (g_sock_fd < 0) {
            LOG_PERROR("socket");
            exit(EXIT_FAILURE);
    }

    memset(&src_addr, 0, sizeof(struct sockaddr_nl));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();

    bind(g_sock_fd, (struct sockaddr*)&src_addr, sizeof(struct sockaddr_nl));

    memset(&dst_addr, 0, sizeof(struct sockaddr_nl));
    dst_addr.nl_family = AF_NETLINK;
    dst_addr.nl_pid = 0; /* send to kernel */
    dst_addr.nl_groups = 0; /* unicast */

    nlh = (struct nlmsghdr*)malloc(NLMSG_SPACE(MAX_PAYLOAD));
    memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;

    strcpy(NLMSG_DATA(nlh), "soc_logd initial message");

    if (sendto(g_sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&dst_addr, sizeof(struct sockaddr_nl)) < 0) {
        LOG_PERROR("sendto");
        free(nlh);
        exit(EXIT_FAILURE);
    }

    free(nlh);

    /* open soc_log device */
    g_fd = open(DEVICE_PATH, O_RDWR);
    if (g_fd < 0) {
        LOG_PERROR("open");
        cleanup_resources();
        return -1;
    }

    // map log buffer and saved to the global data
    mcu_log_buf = (char *)access_buffer(g_fd, MCU_LOG_BUF_FLAG, MCU_LOG_BUF_SIZE);
    g_mapped_bufs[FW_IDX_MCU] = mcu_log_buf;
    g_buf_sizes[FW_IDX_MCU] = MCU_LOG_BUF_SIZE;

    bl1_sbi_log_buf = (char *)access_buffer(g_fd, BL1_SBI_LOG_BUF_FLAG, BL1_SBI_LOG_BUF_SIZE);
    g_mapped_bufs[FW_IDX_BL1_SBI] = bl1_sbi_log_buf;
    g_buf_sizes[FW_IDX_BL1_SBI] = BL1_SBI_LOG_BUF_SIZE;

    uboot_log_buf = (char *)access_buffer(g_fd, UBOOT_LOG_BUF_FLAG, UBOOT_LOG_BUF_SIZE);
    g_mapped_bufs[FW_IDX_UBOOT] = uboot_log_buf;
    g_buf_sizes[FW_IDX_UBOOT] = UBOOT_LOG_BUF_SIZE;

    if (mcu_log_buf == MAP_FAILED || bl1_sbi_log_buf == MAP_FAILED ||
        uboot_log_buf == MAP_FAILED) {
        cleanup_resources();
        return -1;
    }

    LOG_PRINTF("soc log daemon started\n");

    /* write linux boot log */
    write_linux_boot_log(bl1_sbi_log_buf, uboot_log_buf);

    while (running) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(g_sock_fd, &readfds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(g_sock_fd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_PERROR("select");
            break;
        } else if (ret == 0) {
            continue;
        }

        if (FD_ISSET(g_sock_fd, &readfds)) {
            if(netlink_recv_message(g_sock_fd, recv_buf, &recv_len) == 0 ) {
                LOG_PRINTF("recv from kernel: %s\n", recv_buf);
            } else {
                continue;
            }
        } else {
            continue;
        }

        if (strcmp(recv_buf, MCU_LOG_FULL_MSG) == 0) {
            write_to_file(MCU_LOG_FILE, mcu_log_buf, MCU_LOG_BUF_SIZE);
        }
        memset(recv_buf, 0, MAX_PAYLOAD);
    }

    LOG_PRINTF("soc log daemon closing\n");

    // release resource
    cleanup_resources();

    return 0;
}
