/*
  fs_littlefs.c - VFS wrapper/mount for littlefs

  Part of grblHAL

  Copyright (c) 2022-2026 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"

#if FS_ENABLE & FS_LFS

#include "../grbl/protocol.h"
#include "../grbl/platform.h"
#include "../grbl/vfs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "../littlefs/lfs.h"
#include "../littlefs/lfs_util.h"

#define ATTR_MODE 0x6D      // 'm'
#define ATTR_TIMESTAMP 0x74 // 't'

typedef struct time_file {
    lfs_file_t file;
    bool modified;
    time_t timestamp;
    vfs_st_mode_t st_mode;
    struct lfs_attr attrs[2];
    struct lfs_file_config cfg;
} time_file_t;

static struct lfs_dev {
    lfs_t fs;
    const struct lfs_config *config;
} lfs_dev = {0};
static bool is_rootfs;
static char _cwd[51] = "/";
static vfs_path_t cwd = { .name = _cwd, .len = sizeof(_cwd) - 1 };

FLASHMEM static const char *get_path (const char *path)
{
    static vfs_path_t abspath = {0};

    if(strlen(cwd.name) + strlen(path) + 1 > abspath.len) {
        abspath.len = max(50, strlen(cwd.name)) + strlen(path) + 1;
        abspath.name = realloc(abspath.name, abspath.len);
    }

    if(abspath.name) {

        char *newpath;

        if((newpath = malloc(strlen(path) + 1))) {

            strcpy(newpath, path);
            strcpy(abspath.name, *path == '/' ? "/" : cwd.name);

            char *p, *el = strtok(newpath, "/");

            while(el) {
                if(!strcmp("..", el)) {
                    if((p = strrchr(abspath.name, '/')))
                        *(p + (p == abspath.name ? 1 : 0)) = '\0';
                } else if(*el && strcmp(el, ".")) {
                    if(strlen(abspath.name) == 1)
                        strcat(abspath.name, el);
                    else
                        strcat(strcat(abspath.name, "/"), el);
                }
                el = strtok(NULL, "/");
            }

            free(newpath);
        } else
            strcpy(abspath.name, path);
    } else
        abspath.len = 0;

    return abspath.name ? (const char *)abspath.name : path;
}

FLASHMEM static vfs_file_t *fs_open (const char *filename, const char *mode)
{
    int flags = 0;
    vfs_file_t *file = malloc(sizeof(vfs_file_t) + sizeof(time_file_t));

    if(file) {

        time_file_t *f = (time_file_t *)&file->handle;

        // set up description of attributes
        f->modified = false;
        f->timestamp = 0;
        f->st_mode.mode = 0;
        f->attrs[0].type = ATTR_TIMESTAMP;
        f->attrs[0].buffer = &f->timestamp;
        f->attrs[0].size = sizeof(time_t);
        f->attrs[1].type = ATTR_MODE;
        f->attrs[1].buffer = &f->st_mode;
        f->attrs[1].size = sizeof(vfs_st_mode_t);

        // set up config to indicate file has custom attributes
        memset(&f->cfg, 0, sizeof(struct lfs_file_config));
        f->cfg.attrs = f->attrs;
        f->cfg.attr_count = 2;

        while (*mode != '\0') {
            if (*mode == 'r')
                flags |= LFS_O_RDONLY;
            else if (*mode == 'w') {
                flags |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
                if(hal.rtc.get_datetime) {
                    struct tm dt;
                    if(hal.rtc.get_datetime(&dt))
                        f->timestamp = mktime(&dt);
                }
            } else if (*mode == 'a')
                flags |= LFS_O_APPEND;
            mode++;
        }

        if((vfs_errno = lfs_file_opencfg(&lfs_dev.fs, &f->file, get_path(filename), flags, &f->cfg)) != LFS_ERR_OK) {
            free(file);
            file = NULL;
        } else
            file->size = lfs_file_size(&lfs_dev.fs, &f->file);
    }

    return file;
}

FLASHMEM static void fs_close (vfs_file_t *file)
{
    time_file_t *f = (time_file_t *)&file->handle;

    if(f->modified && hal.rtc.get_datetime) {
        struct tm dt;
        if(hal.rtc.get_datetime(&dt))
            f->timestamp = mktime(&dt);
    }

    lfs_file_close(&lfs_dev.fs, &f->file);
    free(file);
}

static size_t fs_read (void *buffer, size_t size, size_t count, vfs_file_t *file)
{
    return lfs_file_read(&lfs_dev.fs, &((time_file_t *)&file->handle)->file, buffer, size * count);
}

static size_t fs_write (const void *buffer, size_t size, size_t count, vfs_file_t *file)
{
    time_file_t *f = (time_file_t *)&file->handle;

    f->modified = true;

    return lfs_file_write(&lfs_dev.fs, &f->file, buffer, size * count);
}

FLASHMEM static size_t fs_tell (vfs_file_t *file)
{
    return lfs_file_tell(&lfs_dev.fs, &((time_file_t *)&file->handle)->file);
}

FLASHMEM static int fs_seek (vfs_file_t *file, size_t offset)
{
    return lfs_file_seek(&lfs_dev.fs, &((time_file_t *)&file->handle)->file, offset, LFS_SEEK_SET);
}

FLASHMEM static bool fs_eof (vfs_file_t *file)
{
    return lfs_file_tell(&lfs_dev.fs, &((time_file_t *)&file->handle)->file) == file->size;
}

static int fs_rename (const char *from, const char *to)
{
    return lfs_rename(&lfs_dev.fs, from, to);
}

FLASHMEM static int fs_unlink (const char *filename)
{
    vfs_stat_t st = {};

    lfs_getattr(&lfs_dev.fs, get_path(filename), ATTR_MODE, &st.st_mode.mode, sizeof(vfs_st_mode_t));

    return st.st_mode.read_only ? -1 : lfs_remove(&lfs_dev.fs, get_path(filename));
}

FLASHMEM static int fs_mkdir (const char *path)
{
    int res;

    if((res = lfs_mkdir(&lfs_dev.fs, get_path(path))) == LFS_ERR_OK) {
        struct tm dt;
        if(hal.rtc.get_datetime && hal.rtc.get_datetime(&dt)) {
            time_t t = mktime(&dt);
            lfs_setattr(&lfs_dev.fs, path, ATTR_TIMESTAMP, &t, sizeof(time_t));
        }
    }

    return res;
}

FLASHMEM static char *fs_getcwd (char *buf, size_t size)
{
    return cwd.name;
}

FLASHMEM static vfs_dir_t *fs_opendir (const char *path)
{
    vfs_dir_t *dir = calloc(1, sizeof(vfs_dir_t) + sizeof(lfs_dir_t));

    if(dir && (vfs_errno = lfs_dir_open(&lfs_dev.fs, (lfs_dir_t *)&dir->handle, path)) != LFS_ERR_OK) {
        free(dir);
        dir = NULL;
    }

    return dir;
}

FLASHMEM static char *fs_readdir (vfs_dir_t *dir, vfs_dirent_t *dirent)
{
    static struct lfs_info f;

    *dirent->name = '\0';

    if((vfs_errno = lfs_dir_read(&lfs_dev.fs, (lfs_dir_t *)&dir->handle, &f)) <= 0)
        return NULL;

    if(!strcmp(f.name, ".") && (vfs_errno = lfs_dir_read(&lfs_dev.fs, (lfs_dir_t *)&dir->handle, &f)) <= 0)
        return NULL;

    if(!strcmp(f.name, "..") && (vfs_errno = lfs_dir_read(&lfs_dev.fs, (lfs_dir_t *)&dir->handle, &f)) <= 0)
        return NULL;

    if(*f.name != '\0')
        strcpy(dirent->name, f.name);

    vfs_errno = 0;
    dirent->size = f.size;
    dirent->st_mode.mode = 0;
    if(!(dirent->st_mode.directory = f.type == LFS_TYPE_DIR))
        lfs_getattr(&lfs_dev.fs, f.name, ATTR_MODE, &dirent->st_mode.mode, sizeof(vfs_st_mode_t));

    return *f.name ? dirent->name : NULL;
}

FLASHMEM static void fs_closedir (vfs_dir_t *dir)
{
    if(dir) {
        vfs_errno = lfs_dir_close(&lfs_dev.fs, (lfs_dir_t *)&dir->handle);
        free(dir);
    }
}

FLASHMEM static int fs_stat (const char *filename, vfs_stat_t *st)
{
    struct lfs_info f;

    if ((vfs_errno = lfs_stat(&lfs_dev.fs, get_path(filename), &f)) == LFS_ERR_OK) {

        st->st_mode.mode = 0;
        st->st_size = f.size;

        if(!(st->st_mode.directory = f.type == LFS_TYPE_DIR))
            lfs_getattr(&lfs_dev.fs, filename, ATTR_MODE, &st->st_mode.mode, sizeof(vfs_st_mode_t));

#ifdef ESP_PLATFORM
        if(lfs_getattr(&lfs_dev.fs, filename, ATTR_TIMESTAMP, &st->st_mtim, sizeof(time_t)) != sizeof(time_t))
            st->st_mtim = (time_t)0;
#else
        if(lfs_getattr(&lfs_dev.fs, filename, ATTR_TIMESTAMP, &st->st_mtime, sizeof(time_t)) != sizeof(time_t))
            st->st_mtime = (time_t)0;
#endif
    } else
        return -1;

    return 0;
}

FLASHMEM static int fs_chdir (const char *path)
{
    int errno;
    vfs_stat_t st;

    if((errno = fs_stat(*path ? path : "/", &st)) == 0) {
        size_t cwdlen;
        if((cwdlen = strlen(path)) > cwd.len) {
            if(cwd.name == _cwd)
                cwd.name = malloc(cwdlen + 1);
            else
                cwd.name = realloc(cwd.name, cwdlen + 1);
            if(cwd.name)
                cwd.len = cwdlen;
            else {
                cwd.name = _cwd;
                cwd.len = sizeof(_cwd) - 1;
                path = "/";
                errno = -1;
            }
        }
        strcpy(cwd.name, *path ? path : "/");
    }

    return errno;
}

FLASHMEM static int fs_chmod (const char *filename, vfs_st_mode_t attr, vfs_st_mode_t mask)
{
    vfs_stat_t st;

    filename = get_path(filename);

    if((vfs_errno = fs_stat(filename, &st)) == 0) {

        mask.directory = Off;
        st.st_mode.mode = (st.st_mode.mode & ~mask.mode) | (attr.mode & mask.mode);

        vfs_errno = lfs_setattr(&lfs_dev.fs, filename, ATTR_MODE, &st.st_mode.mode, sizeof(vfs_st_mode_t));
    }

    return vfs_errno ? -1 : 0;
}

FLASHMEM static int fs_utime (const char *filename, struct tm *modified)
{
    time_t t = mktime(modified);

    return lfs_setattr(&lfs_dev.fs, get_path(filename), ATTR_TIMESTAMP, &t, sizeof(time_t));
}

FLASHMEM static bool fs_getfree (vfs_free_t *free)
{
    free->size = lfs_dev.config->block_count * lfs_dev.config->block_size;
    free->used = lfs_fs_size(&lfs_dev.fs) * lfs_dev.config->block_size;

    return true;
}

FLASHMEM static int fs_format (void)
{
    strcpy(cwd.name, "/");

    return lfs_format(&lfs_dev.fs, lfs_dev.config);
}

FLASHMEM static bool fs_dev_mount (const void *dev, bool mount)
{
    int ret = LFS_ERR_IO;

    if(dev) {

        struct lfs_dev *device = (struct lfs_dev *)dev;

        if(mount)
            ret = lfs_mount(&device->fs, device->config);
        else if(device->config) {
            if((ret = lfs_unmount(&device->fs)) == LFS_ERR_OK)
                device->config = NULL;
        }
    }

    return ret == LFS_ERR_OK;
}

FLASHMEM void fs_littlefs_mount (const char *path, const struct lfs_config *config)
{
    PROGMEM static const vfs_t littlefs = {
        .fs_name = "littlefs",
        .fopen = fs_open,
        .fclose = fs_close,
        .fread = fs_read,
        .fwrite = fs_write,
        .ftell = fs_tell,
        .fseek = fs_seek,
        .feof = fs_eof,
        .frename = fs_rename,
        .funlink = fs_unlink,
        .fmkdir = fs_mkdir,
        .fchdir = fs_chdir,
        .frmdir = fs_unlink,
        .fopendir = fs_opendir,
        .readdir = fs_readdir,
        .fclosedir = fs_closedir,
        .fchmod = fs_chmod,
        .fstat = fs_stat,
        .futime = fs_utime,
        .fgetcwd = fs_getcwd,
        .fgetfree = fs_getfree,
        .format = fs_format,
        .device_mount = fs_dev_mount
    };

    if((lfs_dev.config = config) == NULL)
        return;

    if(lfs_mount(&lfs_dev.fs, config) != LFS_ERR_OK)
        lfs_format(&lfs_dev.fs, config);

    if(lfs_mount(&lfs_dev.fs, config) == LFS_ERR_OK) {
        vfs_st_mode_t mode = {0};
        mode.hidden = settings.fs_options.lfs_hidden;
        is_rootfs = !strcmp(path, "/");
        hal.driver_cap.littlefs = vfs_mount(&lfs_dev, path, &littlefs, mode);
    } else
        task_run_on_startup(report_warning, "LittleFS mount failed!");
}

#endif // LITTLEFS_ENABLE
