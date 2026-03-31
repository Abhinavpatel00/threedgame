#include "fs.h"

#include "external/phyfs/src/physfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define FS_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define FS_MKDIR(path) mkdir(path, 0755)
#endif

#define FS_ORG_NAME "voxelfun"
#define FS_APP_NAME "threedgame"

static int fs_is_absolute(const char* path)
{
    if(!path || !path[0])
        return 0;
#if defined(_WIN32)
    if(((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
        return 1;
#endif
    return path[0] == '/';
}

static void fs_join_path(const char* base, const char* rel, char* out, size_t out_size)
{
    size_t base_len = base ? strlen(base) : 0;
    size_t rel_len  = rel ? strlen(rel) : 0;

    if(base_len + rel_len + 2 > out_size)
    {
        if(out_size > 0)
            out[0] = '\0';
        return;
    }

    if(base_len > 0)
    {
        memcpy(out, base, base_len);
        out[base_len] = '\0';
    }
    else
    {
        out[0] = '\0';
    }

    if(base_len > 0 && out[base_len - 1] != '/' && out[base_len - 1] != '\\')
    {
        out[base_len] = '/';
        out[base_len + 1] = '\0';
        base_len += 1;
    }

    if(rel_len > 0)
        memcpy(out + base_len, rel, rel_len + 1);
}

static bool fs_ensure_host_dir(const char* path)
{
    if(!path || !path[0])
        return false;

    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for(char* p = tmp + 1; *p; ++p)
    {
        if(*p == '/' || *p == '\\')
        {
            char old = *p;
            *p = '\0';
            FS_MKDIR(tmp);
            *p = old;
        }
    }
    FS_MKDIR(tmp);
    return true;
}

bool fs_init(const char* argv0, const char* write_dir, const char* read_dir)
{
    if(!PHYSFS_init(argv0))
    {
        fprintf(stderr, "[fs] PhysFS init failed: %s\n", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return false;
    }

    const char* base_dir = PHYSFS_getBaseDir();
    char        write_path[1024] = {0};
    char        read_path[1024] = {0};

    if(write_dir)
    {
        if(fs_is_absolute(write_dir))
            strncpy(write_path, write_dir, sizeof(write_path) - 1);
        else
            fs_join_path(base_dir, write_dir, write_path, sizeof(write_path));
    }
    else
    {
        const char* pref = PHYSFS_getPrefDir(FS_ORG_NAME, FS_APP_NAME);
        if(pref)
            strncpy(write_path, pref, sizeof(write_path) - 1);
    }

    if(write_path[0] == '\0' && base_dir)
        strncpy(write_path, base_dir, sizeof(write_path) - 1);

    if(write_path[0] != '\0')
    {
        fs_ensure_host_dir(write_path);
        if(!PHYSFS_setWriteDir(write_path))
        {
            fprintf(stderr, "[fs] set write dir failed: %s\n", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            PHYSFS_deinit();
            return false;
        }
    }

    if(read_dir)
    {
        if(fs_is_absolute(read_dir))
            strncpy(read_path, read_dir, sizeof(read_path) - 1);
        else
            fs_join_path(base_dir, read_dir, read_path, sizeof(read_path));
    }
    else if(base_dir)
    {
        strncpy(read_path, base_dir, sizeof(read_path) - 1);
    }

    if(read_path[0] == '\0' || !PHYSFS_mount(read_path, "/", 1))
    {
        fprintf(stderr, "[fs] mount read dir failed: %s\n", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        PHYSFS_deinit();
        return false;
    }

    return true;
}

void fs_shutdown(void)
{
    if(!PHYSFS_deinit())
        fprintf(stderr, "[fs] PhysFS deinit failed: %s\n", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
}

bool fs_read_all(const char* path, void** out_data, size_t* out_size)
{
    if(!path || !out_data || !out_size)
        return false;

    *out_data = NULL;
    *out_size = 0;

    PHYSFS_File* file = PHYSFS_openRead(path);
    if(!file)
        return false;

    PHYSFS_sint64 length = PHYSFS_fileLength(file);
    if(length <= 0)
    {
        PHYSFS_close(file);
        return false;
    }

    void* buffer = malloc((size_t)length);
    if(!buffer)
    {
        PHYSFS_close(file);
        return false;
    }

    PHYSFS_sint64 read_bytes = PHYSFS_readBytes(file, buffer, length);
    PHYSFS_close(file);

    if(read_bytes != length)
    {
        free(buffer);
        return false;
    }

    *out_data = buffer;
    *out_size = (size_t)length;
    return true;
}

bool fs_write_all(const char* path, const void* data, size_t size)
{
    if(!path || (!data && size > 0))
        return false;

    PHYSFS_File* file = PHYSFS_openWrite(path);
    if(!file)
        return false;

    PHYSFS_sint64 written = 0;
    if(size > 0)
        written = PHYSFS_writeBytes(file, data, size);

    PHYSFS_close(file);
    return (written == (PHYSFS_sint64)size);
}

bool fs_exists(const char* path)
{
    if(!path)
        return false;
    return PHYSFS_exists(path) != 0;
}

bool fs_mkdirs(const char* path)
{
    if(!path || !path[0])
        return false;

    char tmp[512];
    size_t len = strlen(path);
    if(len >= sizeof(tmp))
        return false;

    memcpy(tmp, path, len + 1);

    for(char* p = tmp; *p; ++p)
    {
        if(*p == '/')
        {
            *p = '\0';
            if(tmp[0] != '\0')
                PHYSFS_mkdir(tmp);
            *p = '/';
        }
    }

    return PHYSFS_mkdir(tmp) != 0;
}

void fs_free(void* data)
{
    free(data);
}
