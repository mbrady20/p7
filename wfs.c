#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

char *disk;
struct wfs_sb *sb;

struct wfs_inode *get_inode(const char *path)
{
    struct wfs_inode *current = (struct wfs_inode *)(disk + sb->i_blocks_ptr); // set current to the first inode

    char *path_copy = strdup(path);
    if (strcmp(path_copy, "/") == 0)
    {
        return current;
    }

    char *token = strtok(path_copy, "/");

    while (token != NULL)
    {

        if ((current->mode & __S_IFMT) != __S_IFDIR)
        {
            return NULL;
        }

        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[0] * BLOCK_SIZE);

        if (dentry == NULL)
        {
            return NULL;
        }

        int found = 0;
        for (int i = 0; i < current->size / sizeof(struct wfs_dentry); i++)
        {
            if (strcmp(dentry[i].name, token) == 0)
            {
                current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[i].num * sizeof(struct wfs_inode));
                found = 1;
                break;
            }
        }

        if (!found)
        {
            return NULL;
        }

        token = strtok(NULL, "/");
    }

    free(path_copy);
    return current;
}

struct wfs_inode *allocate_inode(int size, const char *path)
{
    struct wfs_inode *root = (struct wfs_inode *)(disk + sb->i_blocks_ptr);
    char *path_copy = strdup(path);

    char *token = strtok(path_copy, "/");
    char *next_token = NULL;
    struct wfs_inode *current = root;

    while (token != NULL)
    {
        next_token = strtok(NULL, "/");
        if (next_token == NULL)
        {
            break;
        }
        if ((current->mode & __S_IFMT) != __S_IFDIR)
        {
            return -1;
        }

        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[0] * BLOCK_SIZE);

        if (dentry == NULL)
        {
            return -1;
        }

        int found = 0;
        for (int i = 0; i < current->size / sizeof(struct wfs_dentry); i++)
        {
            if (strcmp(dentry[i].name, token) == 0)
            {
                current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[i].num * sizeof(struct wfs_inode));
                found = 1;
                break;
            }
        }

        if (!found)
        {
            return -1;
        }

        token = next_token;
    }

    if ((current->mode & __S_IFMT) != __S_IFDIR)
    {
        return -1;
    }

    // current is parent directory, add new directory entry for new inode

    int index = -1;

    if (token == NULL)
    {
        return -1;
    }

    for (int i = 0; i < sb->num_inodes; ++i)
    {
        int offset = (disk + sb->i_bitmap_ptr + i / 8);
        int bit = i % 8;

        int is_set = (offset >> bit) & 1; // this iterates over the bitmap looking for a bit that is not set

        if (!is_set)
        {
            index = i;
            // need to set the bit in the bitmap
            break;
        }
    }

    if (index == -1)
    {
        return -1;
    }

    struct wfs_inode *new_inode = (struct wfs_inode *)(disk + sb->i_blocks_ptr + index * sizeof(struct wfs_inode));
    new_inode->num = index;
    new_inode->size = size;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 1;
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);
}

static int wfs_getattr(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        return -ENOENT;
    }

    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_ctime = inode->ctim;

    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev)
{
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode)
{
    return 0;
}

static int wfs_unlink(const char *path)
{
    return 0;
}

static int wfs_rmdir(const char *path)
{
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    return 0;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    return 0;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    return 0;
}

static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

int main(int argc, char *argv[])
{
    char *disk_path = argv[1];
    int fd = open(disk_path, O_RDWR);
    if (fd == -1)
    {
        perror("Error opening file for reading");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1)
    {
        close(fd);
        perror("Error getting file size");
        return -1;
    }

    disk = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED)
    {
        close(fd);
        perror("Error mmapping the file");
        return -1;
    }

    close(fd);
    sb = (struct wfs_sb *)disk;

    return fuse_main(argc - 1, argv + 1, &ops, NULL);
}