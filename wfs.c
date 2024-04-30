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
    printf("Entering the for loop. Brace yourself!\n");
    for (int i = 0; i < sb->num_data_blocks; ++i)
    {

        char *offset = disk + sb->d_bitmap_ptr + i / 8;
        printf("disk, sb->d_bitmap_ptr, i: %s, %ld, %d\n", disk, sb->d_bitmap_ptr, i);
        printf("Offset: %s\n", offset);
        int bit = i % 8;

        printf("Calculating if bit is set...\n");
        int is_set = (*offset >> bit) & 1; // this iterates over the bitmap looking for a bit that is not set

        if (!is_set)
        {
            printf("Bit is not set. Setting it now...\n");
            *offset |= (1 << bit); // set the bit
            printf("Bit set. Returning block address...\n");
            return sb->d_blocks_ptr + i * BLOCK_SIZE;
        }
        printf("Bit was already set. Moving to next iteration...\n");
    }

    printf("No free blocks found. Returning -1.\n");
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

int resize_inode(int size, struct wfs_inode *inode)
{
    int num_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int current_blocks = (inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    if (num_blocks == current_blocks)
    {
        return 0;
    }

    if (num_blocks < current_blocks)
    {
        for (int i = num_blocks; i < current_blocks; i++)
        {
            remove_block(inode->blocks[i]);
            inode->blocks[i] = 0;
        }
    }
    else
    {
        if (num_blocks > D_BLOCK)
        {
            if (inode->blocks[D_BLOCK] == 0)
            {
                off_t block = allocate_block();
                if (block == -1)
                {
                    return -1;
                }
                inode->blocks[D_BLOCK] = block;
            }
        }
        for (int i = current_blocks; i < num_blocks; i++)
        {
            off_t block = allocate_block();
            if (block == -1)
            {
                return -1;
            }
            if (i < D_BLOCK)
                inode->blocks[i] = block;
            else
            {
                char *ind_block = disk + inode->blocks[D_BLOCK];
                off_t *ind_block_ptr = (off_t *)ind_block;
                ind_block_ptr[i - D_BLOCK] = block;
            }
        }
    }

    inode->size = size;
    return 0;
}

void remove_inode(int index)
{
    char *offset = (disk + sb->i_bitmap_ptr + index / 8);
    int bit = index % 8;
    struct wfs_inode *inode = (struct wfs_inode *)(disk + sb->i_blocks_ptr + index * BLOCK_SIZE);
    for (int i = 0; i < (inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
    {
        remove_block(inode->blocks[i]);
    }
    char *zero = calloc(1, BLOCK_SIZE);
    write(sb->i_blocks_ptr + index * BLOCK_SIZE, zero, BLOCK_SIZE);
    *offset &= ~(1 << bit); // unset the bit
}

struct wfs_inode *get_inode(const char *path)
{
    struct wfs_inode *current = (struct wfs_inode *)(disk + sb->i_blocks_ptr); // set current to the first inode

    char *path_copy = strdup(path);
    printf("path_copy: %s\n", path_copy);
    if (strcmp(path_copy, "/") == 0)
    {
        // print inode data
        printf("inode num: %d mode: %d \n", current->num, current->mode);
        free(path_copy);
        printf("Returning current\n");
        return current;
    }

    char *token = strtok(path_copy, "/");

    while (token != NULL)
    {

        if ((current->mode & __S_IFMT) != __S_IFDIR)
        {
            free(path_copy);
            return NULL;
        }

        int found = 0;

        for (int i = 0; i < (current->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
        {
            printf("current->blocks[i]: %ld\n", current->blocks[i]);
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i]);
            printf("dentry: %s\n", dentry->name);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (strcmp(dentry[j].name, token) == 0)
                {
                    printf("Found matching dentry: %s\n", dentry[j].name);
                    current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[j].num * BLOCK_SIZE);
                    found = 1;
                    break;
                }
            }
        }

        if (!found)
        {
            return NULL;
            free(path_copy);
        }

        token = strtok(NULL, "/");
    }

    free(path_copy);
    return current;
}

struct wfs_inode *get_parent_inode(const char *path)
{
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0)
    {
        // Root does not have a parent in this context, or empty path provided
        return NULL;
    }

    char *path_copy = strdup(path);
    char *last_slash = strrchr(path_copy, '/');

    if (last_slash == path_copy)
    {
        // The path is something like "/filename", parent is root
        free(path_copy);
        return get_inode("/"); // Directly return the root inode
    }
    else if (last_slash != NULL)
    {
        *last_slash = '\0'; // Terminate the path to remove the last component
    }

    struct wfs_inode *parent_inode = get_inode(path_copy); // Get the inode for the modified path
    free(path_copy);
    return parent_inode; // This will be NULL if no such parent exists
}

struct wfs_inode *allocate_inode(int size, const char *path)
{
    printf("Allocating inode for path: %s\n", path);

    struct wfs_inode *root = (struct wfs_inode *)(disk + sb->i_blocks_ptr);
    char *path_copy = strdup(path);

    char *token = strtok(path_copy, "/");
    char *next_token = NULL;
    struct wfs_inode *current = root;

    while (token != NULL)
    {
        printf("Processing token: %s\n", token);
        next_token = strtok(NULL, "/");
        if (next_token == NULL)
        {
            break;
        }

        printf("Next token: %s\n", next_token);

        if ((current->mode & __S_IFMT) != __S_IFDIR)
        {
            printf("Current inode is not a directory\n");
            free(path_copy);
            return NULL;
        }

        int found = 0;

        for (int i = 0; i < (current->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
        {
            printf("current->blocks[i]: %p\n", disk + current->blocks[i]);
            struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (strcmp(dentry[j].name, token) == 0)
                {
                    current = (struct wfs_inode *)(disk + sb->i_blocks_ptr + dentry[j].num * BLOCK_SIZE);
                    found = 1;
                    printf("Found matching dentry: %s\n", dentry[j].name);
                    break;
                }
            }
        }

        if (!found)
        {
            printf("No matching dentry found for token: %s\n", token);
            free(path_copy);
            return NULL;
        }

        token = next_token;
    }

    if ((current->mode & __S_IFMT) != __S_IFDIR)
    {
        printf("Current inode is not a directory\n");
        free(path_copy);
        return NULL;
    }

    int index = -1;

    if (token == NULL)
    {
        printf("Token is NULL\n");
        free(path_copy);
        return NULL;
    }

    for (int i = 1; i < sb->num_inodes; ++i)
    {
        char *offset = (disk + sb->i_bitmap_ptr + i / 8);
        int bit = i % 8;
        printf("index: %d\n", i);
        int is_set = (*offset >> bit) & 1; // this iterates over the bitmap looking for a bit that is not set
        printf("is_set: %d\n", is_set);
        if (!is_set)
        {
            index = i;
            *offset |= (1 << bit); // set the bit
            printf("Found unset bit at index: %d\n", i);
            break;
        }
    }

    if (index == -1)
    {
        printf("No unset bit found in bitmap\n");
        free(path_copy);
        return NULL;
    }

    // current is parent directory, add new directory entry for new inode
    int added = -1;
    for (int i = 0; i < D_BLOCK; i++)
    {
        if (current->blocks[i] == 0)
        {
            resize_inode(current->size + BLOCK_SIZE, current);
        }
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + current->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (dentry[j].num == 0)
            {
                dentry[j].num = index;
                strcpy(dentry[j].name, token);
                added = 1;
                printf("Added new dentry: %s, num: %d\n", dentry[j].name, dentry[j].num);
                goto outer;
            }
        }
    }

outer:
    if (added == -1)
    {
        printf("Failed to add new dentry\n");
        free(path_copy);
        return NULL;
    }

    struct wfs_inode *new_inode = (struct wfs_inode *)(disk + sb->i_blocks_ptr + index * BLOCK_SIZE);
    new_inode->num = index;
    new_inode->size = size;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->nlinks = 1;
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);

    printf("Created new inode with index: %d\n", index);

    // allocate blocks
    int num_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (int i = 0; i < num_blocks; i++)
    {
        off_t block = allocate_block();
        if (block == -1)
        {
            printf("Failed to allocate block\n");
            free(path_copy);
            return NULL;
        }
        new_inode->blocks[i] = block;
        printf("Allocated block: %ld\n", block);
    }
    free(path_copy);
    return new_inode;
}

static int wfs_getattr(const char *path, struct stat *stbuf)
{
    printf("getattr\n");
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
    printf("mknod\n");
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

    inode->mode = mode;

    // Return success
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode)
{
    printf("mkdir\n");
    // Check if an inode already exists at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode != NULL)
    {
        // If an inode already exists, return an error
        return -EEXIST;
    }

    // Allocate a new inode at the given path with the size of a directory entry
    inode = allocate_inode(0, path);
    if (inode == NULL)
    {
        // If the allocation fails (because there's no space left), return an error
        return -ENOSPC;
    }

    // Set the mode of the new inode to directory

    inode->mode = __S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR;
    printf("seg fault????\n");
    // Return success
    return 0;
}

static int wfs_unlink(const char *path)
{
    printf("unlink\n");
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & __S_IFMT) == __S_IFDIR)
    {
        // If the inode is a directory, return an error
        return -EISDIR;
    }

    struct wfs_inode *parent_inode = get_parent_inode(path);
    if (parent_inode == NULL)
    {
        // If no parent inode exists, return an error
        return -ENOENT;
    }

    for (int i = 0; i < D_BLOCK; i++)
    {
        if (parent_inode->blocks[i] == 0)
        {
            continue;
        }
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + parent_inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (dentry[j].num == inode->num)
            {
                dentry[j].num = 0;
                dentry[j].name[0] = '\0';
                break;
            }
        }
    }

    resize_inode(parent_inode->size - sizeof(struct wfs_dentry), parent_inode);

    // Remove the inode
    remove_inode(inode->num);

    // Return success
    return 0;
}

static int wfs_rmdir(const char *path)
{
    printf("rmdir\n");
    // Get the inode at the given path
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & __S_IFMT) != __S_IFDIR)
    {
        // If the inode is not a directory, return an error
        return -ENOTDIR;
    }

    // Remove the inode
    remove_inode(inode->num);

    // Return success
    return 0;
}
static int wfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    printf("read\n");
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        return -ENOENT; // No inode found
    }

    if (offset >= inode->size)
    {
        return 0; // Beyond the end of the file
    }

    int start_block = offset / BLOCK_SIZE;
    int end_block = (offset + size - 1) / BLOCK_SIZE;
    int start_index = offset % BLOCK_SIZE;

    int remaining = size, copied = 0;

    for (int i = start_block; i <= end_block && i < (inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
    {
        int block_offset = (i == start_block) ? start_index : 0;
        int effective_block_size = (i < D_BLOCK) ? BLOCK_SIZE : (BLOCK_SIZE / sizeof(off_t)); // Size of block or number of pointers in indirect block
        // Calculate the block offset and effective block size for each block
        int bytes_to_copy = (BLOCK_SIZE - block_offset) < remaining ? (BLOCK_SIZE - block_offset) : remaining;

        char *block_ptr;
        if (i < D_BLOCK)
        {
            block_ptr = disk + inode->blocks[i];
        }
        else
        {
            char *ind_block = disk + inode->blocks[D_BLOCK];
            off_t *ind_block_ptr = (off_t *)ind_block;
            if (i - D_BLOCK >= effective_block_size)
            {
                continue; // Prevent accessing beyond the allocated pointers
            }
            block_ptr = disk + ind_block_ptr[i - D_BLOCK];
        }

        if (block_ptr == NULL)
        {
            continue; // Skip copying if the block pointer is invalid
        }

        memcpy(buf + copied, block_ptr + block_offset, bytes_to_copy);
        remaining -= bytes_to_copy;
        printf("Copied %d bytes\n", bytes_to_copy);
        copied += bytes_to_copy;
    }

    return copied;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("write\n");
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        return -ENOENT; // No inode found
    }

    // Ensure the inode size is large enough to handle the write
    if (offset + size > inode->size)
    {
        if (resize_inode(offset + size, inode) != 0)
        {
            return -ENOSPC; // Unable to resize inode
        }
    }

    int start_block = offset / BLOCK_SIZE;
    int end_block = (offset + size - 1) / BLOCK_SIZE;
    int bytes_written = 0;

    for (int i = start_block; i <= end_block; i++)
    {
        int block_offset = (i == start_block) ? (offset % BLOCK_SIZE) : 0;
        int block_end = (i == end_block) ? ((offset + size) % BLOCK_SIZE) : BLOCK_SIZE;
        if (block_end == 0 && i == end_block)
            block_end = BLOCK_SIZE;
        char *block_ptr;
        if (i < D_BLOCK)
            block_ptr = disk + inode->blocks[i];
        else
        {
            char *ind_block = disk + inode->blocks[D_BLOCK];
            off_t *ind_block_ptr = (off_t *)ind_block;
            block_ptr = disk + ind_block_ptr[i - D_BLOCK];
        }
        char temp_buf[BLOCK_SIZE];
        memcpy(temp_buf, block_ptr, BLOCK_SIZE); // Read current block content

        int copy_size = (block_end - block_offset);
        if (i == end_block)
        {
            copy_size = size - bytes_written; // Adjust copy size for the last block
        }
        if (copy_size > (size - bytes_written))
        {
            copy_size = size - bytes_written; // Ensure we do not exceed the total remaining bytes to write
        }

        memcpy(temp_buf + block_offset, buf + bytes_written, copy_size);
        memcpy(block_ptr, temp_buf, BLOCK_SIZE);
        bytes_written += copy_size;
    }

    return bytes_written;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
    printf("readdir\n");
    // Get the inode at the given path
    printf("path: %s\n", path);
    struct wfs_inode *inode = get_inode(path);
    if (inode == NULL)
    {
        // If no inode exists at the path, return an error
        return -ENOENT;
    }

    // Check if the inode is a directory
    if ((inode->mode & __S_IFMT) != __S_IFDIR)
    {
        // If the inode is not a directory, return an error
        return -ENOTDIR;
    }

    // Iterate over the entries in the directory
    for (int i = 0; i < (inode->size + BLOCK_SIZE - 1) / BLOCK_SIZE; i++)
    {
        struct wfs_dentry *dentry = (struct wfs_dentry *)(disk + inode->blocks[i]);
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
        {
            if (dentry[j].num != 0)
            {
                if (filler(buf, dentry[j].name, NULL, 0))
                {
                    break;
                }
            }
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

    disk = (char *)mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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