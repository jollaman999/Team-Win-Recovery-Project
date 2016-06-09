/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

extern "C" {
#include "mtdutils/mtdutils.h"
}

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

#include <android-base/file.h>

#include "bootloader.h"
#include "common.h"
#include "mtdutils/mtdutils.h"
//#include "roots.h"
//#include "unique_fd.h"

// fake Volume struct that allows us to use the AOSP code easily
struct Volume
{
    char fs_type[8];
    char blk_device[256];
};

static Volume misc;

void set_misc_device(const char* type, const char* name) {
    strlcpy(misc.fs_type, type, sizeof(misc.fs_type));
    if (strlen(name) >= sizeof(misc.blk_device)) {
        LOGE("New device name of '%s' is too large for bootloader.cpp\n", name);
    } else {
        strcpy(misc.blk_device, name);
    }
}

static int get_bootloader_message_mtd(bootloader_message* out, const Volume* v);
static int set_bootloader_message_mtd(const bootloader_message* in, const Volume* v);
static bool read_misc_partition(const Volume* v, size_t offset, size_t size, std::string* out);
static bool write_misc_partition(const Volume* v, size_t offset, const std::string& in);

int get_bootloader_message(bootloader_message* out) {
#if 0
    Volume* v = volume_for_path("/misc");
    if (v == nullptr) {
        LOGE("Cannot load volume /misc!\n");
        return -1;
    }
#else
    Volume* v = &misc;
    if (v->fs_type[0] == 0) {
        LOGI("Not using /misc, not defined in fstab.\n");
        return -1;
    }
#endif
    if (strcmp(v->fs_type, "mtd") == 0) {
        return get_bootloader_message_mtd(out, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        std::string s;
        if (!read_misc_partition(v, BOOTLOADER_MESSAGE_OFFSET_IN_MISC, sizeof(bootloader_message),
                                 &s)) {
            return -1;
        }
        memcpy(out, s.data(), s.size());
        return 0;
    }
    LOGE("Unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
}

bool read_wipe_package(size_t size, std::string* out) {
#if 0
    Volume* v = volume_for_path("/misc");
    if (v == nullptr) {
        LOGE("Cannot load volume /misc!\n");
        return false;
    }
#else
    Volume* v = &misc;
    if (v->fs_type[0] == 0) {
        LOGI("Not using /misc, not defined in fstab.\n");
        return false;
    }
#endif
    if (strcmp(v->fs_type, "mtd") == 0) {
        LOGE("Read wipe package on mtd is not supported.\n");
        return false;
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        return read_misc_partition(v, WIPE_PACKAGE_OFFSET_IN_MISC, size, out);
    }
    LOGE("Unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return false;
}

int set_bootloader_message(const bootloader_message* in) {
#if 0
    Volume* v = volume_for_path("/misc");
    if (v == nullptr) {
        LOGE("Cannot load volume /misc!\n");
        return -1;
    }
#else
    Volume* v = &misc;
    if (v->fs_type[0] == 0) {
        LOGI("Not using /misc, not defined in fstab.\n");
        return -1;
    }
#endif
    if (strcmp(v->fs_type, "mtd") == 0) {
        return set_bootloader_message_mtd(in, v);
    } else if (strcmp(v->fs_type, "emmc") == 0) {
        std::string s(reinterpret_cast<const char*>(in), sizeof(*in));
        bool success = write_misc_partition(v, BOOTLOADER_MESSAGE_OFFSET_IN_MISC, s);
        return success ? 0 : -1;
    }
    LOGE("Unknown misc partition fs_type \"%s\"\n", v->fs_type);
    return -1;
}

// ------------------------------
// for misc partitions on MTD
// ------------------------------

static const int MISC_PAGES = 3;         // number of pages to save
static const int MISC_COMMAND_PAGE = 1;  // bootloader command is this page

static int get_bootloader_message_mtd(bootloader_message* out,
                                      const Volume* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition* part = mtd_find_partition_by_name(v->blk_device);
    if (part == nullptr || mtd_partition_info(part, nullptr, nullptr, &write_size)) {
        LOGE("failed to find \"%s\"\n", v->blk_device);
        return -1;
    }

    MtdReadContext* read = mtd_read_partition(part);
    if (read == nullptr) {
        LOGE("failed to open \"%s\": %s\n", v->blk_device, strerror(errno));
        return -1;
    }

    const ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("failed to read \"%s\": %s\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(out, &data[write_size * MISC_COMMAND_PAGE], sizeof(*out));
    return 0;
}
static int set_bootloader_message_mtd(const bootloader_message* in,
                                      const Volume* v) {
    size_t write_size;
    mtd_scan_partitions();
    const MtdPartition* part = mtd_find_partition_by_name(v->blk_device);
    if (part == nullptr || mtd_partition_info(part, nullptr, nullptr, &write_size)) {
        LOGE("failed to find \"%s\"\n", v->blk_device);
        return -1;
    }

    MtdReadContext* read = mtd_read_partition(part);
    if (read == nullptr) {
        LOGE("failed to open \"%s\": %s\n", v->blk_device, strerror(errno));
        return -1;
    }

    ssize_t size = write_size * MISC_PAGES;
    char data[size];
    ssize_t r = mtd_read_data(read, data, size);
    if (r != size) LOGE("failed to read \"%s\": %s\n", v->blk_device, strerror(errno));
    mtd_read_close(read);
    if (r != size) return -1;

    memcpy(&data[write_size * MISC_COMMAND_PAGE], in, sizeof(*in));

    MtdWriteContext* write = mtd_write_partition(part);
    if (write == nullptr) {
        LOGE("failed to open \"%s\": %s\n", v->blk_device, strerror(errno));
        return -1;
    }
    if (mtd_write_data(write, data, size) != size) {
        LOGE("failed to write \"%s\": %s\n", v->blk_device, strerror(errno));
        mtd_write_close(write);
        return -1;
    }
    if (mtd_write_close(write)) {
        LOGE("failed to finish \"%s\": %s\n", v->blk_device, strerror(errno));
        return -1;
    }

    LOGI("Set boot command \"%s\"\n", in->command[0] != 255 ? in->command : "");
    return 0;
}


// ------------------------------------
// for misc partitions on block devices
// ------------------------------------

static void wait_for_device(const char* fn) {
    int tries = 0;
    int ret;
    do {
        ++tries;
        struct stat buf;
        ret = stat(fn, &buf);
        if (ret == -1) {
            printf("failed to stat \"%s\" try %d: %s\n", fn, tries, strerror(errno));
            sleep(1);
        }
    } while (ret && tries < 10);

    if (ret) {
        printf("failed to stat \"%s\"\n", fn);
    }
}

static bool read_misc_partition(const Volume* v, size_t offset, size_t size, std::string* out) {
    wait_for_device(v->blk_device);
    int fd = open(v->blk_device, O_RDONLY);
    if (fd == -1) {
        LOGE("Failed to open \"%s\": %s\n", v->blk_device, strerror(errno));
        return false;
    }
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(offset)) {
        LOGE("Failed to lseek \"%s\": %s\n", v->blk_device, strerror(errno));
        close(fd);
        return false;
    }
    out->resize(size);
    if (!android::base::ReadFully(fd, &(*out)[0], size)) {
        LOGE("Failed to read \"%s\": %s\n", v->blk_device, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

static bool write_misc_partition(const Volume* v, size_t offset, const std::string& in) {
    wait_for_device(v->blk_device);
    int fd = open(v->blk_device, O_WRONLY | O_SYNC);
    if (fd == -1) {
        LOGE("Failed to open \"%s\": %s\n", v->blk_device, strerror(errno));
        return false;
    }
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(offset)) {
        LOGE("Failed to lseek \"%s\": %s\n", v->blk_device, strerror(errno));
        close(fd);
        return false;
    }
    if (!android::base::WriteFully(fd, in.data(), in.size())) {
        LOGE("Failed to write \"%s\": %s\n", v->blk_device, strerror(errno));
        close(fd);
        return false;
    }

    if (fsync(fd) == -1) {
        LOGE("Failed to fsync \"%s\": %s\n", v->blk_device, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);
    return true;
}


static const char *COMMAND_FILE = "/cache/recovery/command";
static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", (int)sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", (int)sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *token;
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                token = strtok(buf, "\r\n");
                if (token != NULL) {
                    (*argv)[*argc] = strdup(token);  // Strip newline.
                } else {
                    --*argc;
                }
            }

            fflush(fp);
            if (ferror(fp)) LOGE("Error in %s\n(%s)\n", COMMAND_FILE, strerror(errno));
            fclose(fp);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}
