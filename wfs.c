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

off_t allocate_block()
{
    for (int i = 0; i < sb->num_data_blocks; ++i)
    {
        char *offset = disk + sb->d_bitmap_ptr + i / 8;
        int bit = i % 8;

        int is_set = (*offset >> bit) & 1; // this iterates over the bitmap looking for a bit that is not set

        if (!is_set)
        {
            *offset |= (1 << bit); // set the bit
            return sb->d_blocks_ptr + i * BLOCK_SIZE;
        }
    }

    return -1; // return -1 instead of NULL because the return type is off_t
}

void remove_block(off_t block)
{
    int index = (block - sb->d_blocks_ptr) / BLOCK_SIZE;
    char *offset = (disk + sb->d_bitmap_ptr + index / 8);
    char *zero = calloc(1, BLOCK_SIZE);
    write(block, zero, BLOCK_SIZE);
    int bit = index % 8;
    *offset &= ~(1 << bit); // unset the bit
}

void remove_inode(int index)
{
    char *offset = (disk + sb->i_bitmap_ptr + index / 8);
    int bit = index % 8;
    struct wfs_inode *inode = (struct wfs_inode *)(disk + sb->i_blocks_ptr + index * sizeof(struct wfs_inode));
    for (int i = 0; i < (inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
    {
        remove_block(inode->blocks[i]);
    }
    char *zero = calloc(1, sizeof(struct wfs_inode));
    write(sb->i_blocks_ptr + index * sizeof(struct wfs_inode), zero, sizeof(struct wfs_inode));
    *offset &= ~(1 << bit); // unset the bit
}

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

        int found = 0;
        for (int i = 0; i < (current->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
        {
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (strcmp(dentry[j].name, token) == 0)
                {
                    current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[j].num * sizeof(struct wfs_inode));
                    found = 1;
                    break;
                }
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
            return NULL;
        }

        int found = 0;
        for (int i = 0; i < (current->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
        {
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (strcmp(dentry[j].name, token) == 0)
                {
                    current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[j].num * sizeof(struct wfs_inode));
                    found = 1;
                    break;
                }
            }
        }

        if (!found)
        {
            return NULL;
        }

        token = next_token;
    }

    if ((current->mode & __S_IFMT) != __S_IFDIR)
    {
        return NULL;
    }

    int index = -1;

    if (token == NULL)
    {
        return NULL;
    }

    for (int i = 0; i < sb->num_inodes; ++i)
    {
        char *offset = (disk + sb->i_bitmap_ptr + i / 8);
        int bit = i % 8;

        int is_set = (*offset >> bit) & 1; // this iterates over the bitmap looking for a bit that is not set

        if (!is_set)
        {
            index = i;
            *offset |= (1 << bit); // set the bit
            break;
        }
    }

    if (index == -1)
    {
        return NULL;
    }

    // current is parent directory, add new directory entry for new inode
    int added = -1;
    for (int i = 0; i < (size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
    {
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i] * BLOCK_SIZE);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (dentry[j].num == 0)
            {
                dentry[j].num = index;
                strcpy(dentry[j].name, token);
                added = 1;
                break;
            }
        }
    }
    if (added == -1)
    {
        return NULL;
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

    // allocate blocks
    int num_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (int i = 0; i < num_blocks; i++)
    {
        off_t block = allocate_block();
        if (block == -1)
        {
            return NULL;
        }
        new_inode->blocks[i] = block;
    }

    return new_inode;
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

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    // Check if an inode already exists at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode != NULL)
    {
        // If an inode already exists, return an error
        return -EEXIST;
    }

    // Allocate a new inode at the given path
    inode = allocate_inode(0, path);
    if (inode == NULL)
    {
        // If the allocation fails (because there's no space left), return an error
        return -ENOSPC;
    }

    // Set the mode and device of the new inode
    inode->mode = mode;
    inode->rdev = rdev;

    // Return success
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode)
{
    // Check if an inode already exists at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode != NULL)
    {
        // If an inode already exists, return an error
        return -EEXIST;
    }

    // Allocate a new inode at the given path with the size of a directory entry
    inode = allocate_inode(sizeof(struct wfs_dentry), path);
    if (inode == NULL)
    {
        // If the allocation fails (because there's no space left), return an error
        return -ENOSPC;
    }

    // Set the mode of the new inode to directory
    inode->mode = S_IFDIR | mode;

    // Return success
    return 0;
}

static int wfs_unlink(const char *path)
{
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & S_IFMT) == S_IFDIR)
    {
        // If the inode is a directory, return an error
        return -EISDIR;
    }

    // Remove the inode
    remove_inode(inode->num);

    // Return success
    return 0;
}

static int wfs_rmdir(const char *path)
{
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & S_IFMT) != S_IFDIR)
    {
        // If the inode is not a directory, return an error
        return -ENOTDIR;
    }

    // Check if the directory is empty
    if (is_directory_empty(inode))
    {
        // If the directory is not empty, return an error
        return -ENOTEMPTY;
    }

    // Remove the inode
    remove_inode(inode->num);

    // Return success
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the offset is beyond the end of the file
    if (offset >= inode->size)
    {
        // If the offset is beyond the end of the file, return 0 (indicating end of file)
        return 0;
    }

    // If the read would go beyond the end of the file, truncate it
    if (offset + size > inode->size)
    {
        size = inode->size - offset;
    }

    // Copy the data from the inode's data to the buffer
    memcpy(buf, inode->data + offset, size);

    // Return the number of bytes read
    return size;
}

static int wfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the write would go beyond the end of the file
    if (offset + size > inode->size)
    {
        // If the write would go beyond the end of the file, resize the file
        if (resize_inode(inode, offset + size) != 0)
        {
            // If the resize fails, return an error
            return -EFBIG;
        }
    }

    // Copy the data from the buffer to the inode's data
    memcpy(inode->data + offset, buf, size);

    // Return the number of bytes written
    return size;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & S_IFMT) != S_IFDIR)
    {
        // If the inode is not a directory, return an error
        return -ENOTDIR;
    }

    // Iterate over the entries in the directory
    struct wfs_dentry *dentry = (struct wfs_dentry *)inode->data;
    for (int i = 0; i < inode->size / sizeof(struct wfs_dentry); i++)
    {
        // Add the entry to the readdir buffer
        if (filler(buf, dentry[i].name, NULL, 0))
        {
            // If the buffer is full, stop
            break;
        }
    }

    // Return success
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

    printf("arg 1: %s\n", argv[0]);
    return fuse_main(argc - 1, argv + 1, &ops, NULL);
}