/*

  MyFS: a tiny file-system written for educational purposes

  MyFS is

  Copyright 2018-21 by

  University of Alaska Anchorage, College of Engineering.

  Copyright 2022-24

  University of Texas at El Paso, Department of Computer Science.

  Contributors: Christoph Lauter, Bryan Perez, Patrick Perez.

  and based on

  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall myfs.c implementation.c `pkg-config fuse --cflags --libs` -o myfs

*/

#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

/* Constants */
#define MYFS_MAGIC_NUMBER 0x12345678
#define MYFS_BLOCK_SIZE 1024
#define MYFS_MAX_NAME_LEN 255

/* Helper Macros */
#define TO_OFFSET(ptr, fsptr) ((size_t)((char *)(ptr) - (char *)(fsptr)))
#define TO_PTR(offset, fsptr) ((void *)((char *)(fsptr) + (offset)))

/* File Types */
typedef enum {
    MYFS_FILE = 1,
    MYFS_DIR = 2
} myfs_node_type_t;

/* Structures */
typedef struct myfs_handle {
    uint32_t magic;
    size_t free_offset;
    size_t root_dir;
    size_t total_size;
    size_t block_size;
} myfs_handle_t;

typedef struct myfs_node {
    myfs_node_type_t type;
    char name[MYFS_MAX_NAME_LEN + 1];
    size_t parent_offset;
    size_t child_offset;
    size_t next_sibling_offset;
    size_t size;
    size_t data_offset;
    struct timespec atim;
    struct timespec mtim;
} myfs_node_t;

/* Helper Method Prototypes */
void myfs_initialize(void *fsptr, size_t fssize);
void *myfs_alloc(myfs_handle_t *handle, void *fsptr, size_t size);
const char *myfs_get_basename(const char *path);
myfs_node_t *myfs_find_node(void *fsptr, myfs_handle_t *handle, const char *path);
myfs_node_t *myfs_find_parent_node(void *fsptr, myfs_handle_t *handle, const char *path);
void myfs_add_child(void *fsptr, myfs_node_t *parent, myfs_node_t *child);
int myfs_count_children(void *fsptr, myfs_node_t *dir);
void myfs_update_atime(myfs_node_t *node);
void myfs_update_mtime(myfs_node_t *node);

/* Helper types and functions */

/**
 * Initializes the in-memory filesystem.
 *
 * @param fsptr Pointer to the filesystem memory.
 * @param fssize Size of the memory space for the filesystem.
 */
void myfs_initialize(void *fsptr, size_t fssize) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // check if the filesystem is already initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        memset(fsptr, 0, fssize);

        // set magic number to indicate valid filesystem
        handle->magic = MYFS_MAGIC_NUMBER;

        // initialize filesystem metadata
        handle->free_offset = sizeof(myfs_handle_t);
        handle->root_dir = handle->free_offset;
        handle->total_size = fssize;
        handle->block_size = MYFS_BLOCK_SIZE;

        // create and configure the root directory
        myfs_node_t *root = (myfs_node_t *)TO_PTR(handle->free_offset, fsptr);
        memset(root, 0, sizeof(myfs_node_t));
        root->type = MYFS_DIR;
        strncpy(root->name, "/", MYFS_MAX_NAME_LEN);

        // set current time for root directory
        clock_gettime(CLOCK_REALTIME, &root->atim);
        clock_gettime(CLOCK_REALTIME, &root->mtim);

        // update the free offset
        handle->free_offset += sizeof(myfs_node_t);
    }
}

/**
 * Allocates memory for a new object in the filesystem.
 *
 * @param handle Pointer to the filesystem handle.
 * @param fsptr Pointer to the filesystem memory.
 * @param size Size of the memory to allocate.
 * @return Pointer to the allocated memory or NULL if insufficient space.
 */
void *myfs_alloc(myfs_handle_t *handle, void *fsptr, size_t size) {
    // align the free offset to the nearest boundary
    size_t aligned_offset = (handle->free_offset + sizeof(void *) - 1) & ~(sizeof(void *) - 1);

    // check if there is enough space
    if (aligned_offset + size > handle->total_size) {
        return NULL;
    }

    // allocate and zero-initialize memory
    void *allocated = TO_PTR(aligned_offset, fsptr);
    memset(allocated, 0, size);

    // update free offset
    handle->free_offset = aligned_offset + size;
    return allocated;
}

/**
 * Extracts the basename from a given path.
 *
 * @param path The full path.
 * @return The basename of the path, or NULL if invalid.
 */
const char *myfs_get_basename(const char *path) {
    // find the last slash in the path
    const char *last_slash = strrchr(path, '/');
    return (last_slash && *(last_slash + 1)) ? last_slash + 1 : NULL;
}

/**
 * Finds a node (file or directory) corresponding to the given path.
 *
 * @param fsptr Pointer to the filesystem memory.
 * @param handle Pointer to the filesystem handle.
 * @param path The path of the node to find.
 * @return Pointer to the node, or NULL if not found.
 */
myfs_node_t *myfs_find_node(void *fsptr, myfs_handle_t *handle, const char *path) {
    // handle root directory as a special case
    if (strcmp(path, "/") == 0) {
        return (myfs_node_t *)TO_PTR(handle->root_dir, fsptr);
    }

    // tokenize the path to traverse
    char *path_copy = strdup(path);
    if (!path_copy) return NULL;

    myfs_node_t *current = (myfs_node_t *)TO_PTR(handle->root_dir, fsptr);
    char *token, *saveptr;

    // traverse the path tokens
    token = strtok_r(path_copy, "/", &saveptr);
    while (token && current) {
        // ensure the current node is a directory
        if (current->type != MYFS_DIR) {
            free(path_copy);
            return NULL;
        }

        size_t child_offset = current->child_offset;
        current = NULL;

        // search among the children
        while (child_offset) {
            myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
            if (strcmp(child->name, token) == 0) {
                current = child;
                break;
            }
            child_offset = child->next_sibling_offset;
        }

        token = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);
    return current;
}

/**
 * Finds the parent directory of a given path.
 *
 * @param fsptr Pointer to the filesystem memory.
 * @param handle Pointer to the filesystem handle.
 * @param path The path of the child node.
 * @return Pointer to the parent node, or NULL if not found.
 */
myfs_node_t *myfs_find_parent_node(void *fsptr, myfs_handle_t *handle, const char *path) {
    // handle root directory as a special case
    if (strcmp(path, "/") == 0) return NULL;

    char *path_copy = strdup(path);
    if (!path_copy) return NULL;

    char *last_slash = strrchr(path_copy, '/');
    if (!last_slash || last_slash == path_copy) {
        free(path_copy);
        return (myfs_node_t *)TO_PTR(handle->root_dir, fsptr);
    }

    *last_slash = '\0';
    myfs_node_t *parent = myfs_find_node(fsptr, handle, path_copy);

    free(path_copy);
    return parent;
}

/**
 * Adds a child node to the parent's child list.
 *
 * @param fsptr Pointer to the filesystem memory.
 * @param parent Pointer to the parent node.
 * @param child Pointer to the child node.
 */
void myfs_add_child(void *fsptr, myfs_node_t *parent, myfs_node_t *child) {
    child->parent_offset = TO_OFFSET(parent, fsptr);
    child->next_sibling_offset = parent->child_offset;
    parent->child_offset = TO_OFFSET(child, fsptr);
}

/**
 * Counts the number of children in a directory node.
 *
 * @param fsptr Pointer to the filesystem memory.
 * @param dir Pointer to the directory node.
 * @return The number of children.
 */
int myfs_count_children(void *fsptr, myfs_node_t *dir) {
    size_t child_offset = dir->child_offset;
    int count = 0;

    // traverse the child list
    while (child_offset) {
        count++;
        myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
        child_offset = child->next_sibling_offset;
    }

    return count;
}

/**
 * Updates the access time of a node to the current time.
 *
 * @param node Pointer to the node.
 */
void myfs_update_atime(myfs_node_t *node) {
    if (node) {
        clock_gettime(CLOCK_REALTIME, &node->atim);
    }
}

/**
 * Updates the modification time of a node to the current time.
 *
 * @param node Pointer to the node.
 */
void myfs_update_mtime(myfs_node_t *node) {
    if (node) {
        clock_gettime(CLOCK_REALTIME, &node->mtim);
    }
}


/* End of helper functions */


/* Implements an emulation of the stat system call on the filesystem
   of size fssize pointed to by fsptr.

   If path can be followed and describes a file or directory
   that exists and is accessable, the access information is
   put into stbuf.

   On success, 0 is returned. On failure, -1 is returned and
   the appropriate error code is put into *errnoptr.

   man 2 stat documents all possible error codes and gives more detail
   on what fields of stbuf need to be filled in. Essentially, only the
   following fields need to be supported:

   st_uid      the value passed in argument
   st_gid      the value passed in argument
   st_mode     (as fixed values S_IFDIR | 0755 for directories,
                                S_IFREG | 0755 for files)
   st_nlink    (as many as there are subdirectories (not files) for directories
                (including . and ..),
                1 for files)
   st_size     (supported only for files, where it is the real file size)
   st_atim
   st_mtim

*/
int __myfs_getattr_implem(void *fsptr, size_t fssize, int *errnoptr, uid_t uid, gid_t gid, const char *path, struct stat *stbuf) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        myfs_initialize(fsptr, fssize);
    }

    // special case: handle root directory
    if (strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat)); // clear the stat structure
        stbuf->st_uid = uid;                  // set user id for the root directory
        stbuf->st_gid = gid;                  // set group id for the root directory
        stbuf->st_mode = S_IFDIR | 0755;      // set directory mode with rwxr-xr-x permissions
        stbuf->st_nlink = 2;                  // root has two links: "." and ".."
        return 0;                             // success
    }

    // locate the node corresponding to the given path
    myfs_node_t *node = myfs_find_node(fsptr, handle, path);
    if (!node) {
        *errnoptr = ENOENT; // path does not exist
        return -1;          // failure
    }

    // fill the stat structure for the found node
    memset(stbuf, 0, sizeof(struct stat)); // clear the stat structure
    stbuf->st_uid = uid;                  // set user id for the node
    stbuf->st_gid = gid;                  // set group id for the node
    stbuf->st_atim = node->atim;          // set access time from the node
    stbuf->st_mtim = node->mtim;          // set modification time from the node

    if (node->type == MYFS_DIR) {
        // if the node is a directory
        stbuf->st_mode = S_IFDIR | 0755;  // set directory mode with rwxr-xr-x permissions
        stbuf->st_nlink = 2;              // directories have two links: "." and ".."
    } else if (node->type == MYFS_FILE) {
        // if the node is a regular file
        stbuf->st_mode = S_IFREG | 0755;  // set regular file mode with rwxr-xr-x permissions
        stbuf->st_nlink = 1;              // regular files have a single link
        stbuf->st_size = node->size;      // set file size
    } else {
        *errnoptr = EINVAL;               // invalid node type
        return -1;                        // failure
    }

    return 0; // success
}

/* Implements an emulation of the readdir system call on the filesystem
   of size fssize pointed to by fsptr.

   If path can be followed and describes a directory that exists and
   is accessable, the names of the subdirectories and files
   contained in that directory are output into *namesptr. The . and ..
   directories must not be included in that listing.

   If it needs to output file and subdirectory names, the function
   starts by allocating (with calloc) an array of pointers to
   characters of the right size (n entries for n names). Sets
   *namesptr to that pointer. It then goes over all entries
   in that array and allocates, for each of them an array of
   characters of the right size (to hold the i-th name, together
   with the appropriate '\0' terminator). It puts the pointer
   into that i-th array entry and fills the allocated array
   of characters with the appropriate name. The calling function
   will call free on each of the entries of *namesptr and
   on *namesptr.

   The function returns the number of names that have been
   put into namesptr.

   If no name needs to be reported because the directory does
   not contain any file or subdirectory besides . and .., 0 is
   returned and no allocation takes place.

   On failure, -1 is returned and the *errnoptr is set to
   the appropriate error code.

   The error codes are documented in man 2 readdir.

   In the case memory allocation with malloc/calloc fails, failure is
   indicated by returning -1 and setting *errnoptr to EINVAL.

*/
int __myfs_readdir_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path, char ***namesptr) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // locate the directory node for the given path
    myfs_node_t *dir = myfs_find_node(fsptr, handle, path);
    if (!dir) {
        *errnoptr = ENOENT; // directory not found
        return -1;          // return failure
    }

    // ensure the node is a directory
    if (dir->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // path is not a directory
        return -1;           // return failure
    }

    // update the directory's access time
    myfs_update_atime(dir);

    // count the number of children in the directory
    size_t child_offset = dir->child_offset;
    int count = 0;
    while (child_offset) {
        myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
        count++; // increment the child count
        child_offset = child->next_sibling_offset; // move to the next sibling
    }

    // if the directory is empty, set namesptr to NULL and return 0
    if (count == 0) {
        *namesptr = NULL; // no entries to return
        return 0;         // success with zero entries
    }

    // allocate memory for the array of child names
    char **names = calloc(count, sizeof(char *));
    if (!names) {
        *errnoptr = ENOMEM; // memory allocation failed
        return -1;          // return failure
    }

    // populate the names array with child names
    child_offset = dir->child_offset; // reset to the first child
    int index = 0;
    while (child_offset) {
        myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
        // duplicate the child name into the names array
        names[index] = strdup(child->name);
        if (!names[index]) {
            *errnoptr = ENOMEM; // memory allocation failed
            // free all previously allocated memory
            for (int i = 0; i < index; i++) {
                free(names[i]);
            }
            free(names); // free the names array itself
            return -1;   // return failure
        }
        index++;                       // move to the next entry
        child_offset = child->next_sibling_offset; // move to the next sibling
    }

    // set the output pointer to the names array
    *namesptr = names;
    return count; // return the number of entries in the directory
}

/* Implements an emulation of the mknod system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the creation of regular files.

   If a file gets created, it is of size zero and has default
   ownership and mode bits.

   The call creates the file indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mknod.
*/
int __myfs_mknod_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // validate the filename length
    const char *filename = myfs_get_basename(path);
    if (!filename || strlen(filename) > MYFS_MAX_NAME_LEN) {
        *errnoptr = ENAMETOOLONG; // filename exceeds maximum allowed length
        return -1;                // return failure
    }

    // locate the parent directory of the new file
    myfs_node_t *parent = myfs_find_parent_node(fsptr, handle, path);
    if (!parent) {
        *errnoptr = ENOENT; // parent directory does not exist
        return -1;          // return failure
    }

    // ensure the parent is a directory
    if (parent->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // parent is not a directory
        return -1;           // return failure
    }

    // check if a file with the same name already exists
    size_t child_offset = parent->child_offset;
    while (child_offset) {
        myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
        if (strcmp(child->name, filename) == 0) {
            *errnoptr = EEXIST; // file already exists
            return -1;          // return failure
        }
        child_offset = child->next_sibling_offset; // move to the next sibling
    }

    // allocate memory for the new file node
    myfs_node_t *new_file = myfs_alloc(handle, fsptr, sizeof(myfs_node_t));
    if (!new_file) {
        *errnoptr = ENOSPC; // not enough space in the filesystem
        return -1;          // return failure
    }

    // initialize the new file node
    strncpy(new_file->name, filename, MYFS_MAX_NAME_LEN); // copy filename
    new_file->name[MYFS_MAX_NAME_LEN] = '\0';             // ensure null termination
    new_file->type = MYFS_FILE;                           // set type to regular file
    new_file->size = 0;                                   // initialize size to 0
    new_file->data_offset = 0;                            // no data allocated initially
    clock_gettime(CLOCK_REALTIME, &new_file->atim);       // set access time
    clock_gettime(CLOCK_REALTIME, &new_file->mtim);       // set modification time
    new_file->parent_offset = TO_OFFSET(parent, fsptr);   // set parent offset
    new_file->next_sibling_offset = parent->child_offset; // link to parent's child list

    // link the new file to the parent directory
    parent->child_offset = TO_OFFSET(new_file, fsptr); // update parent's child pointer

    return 0; // success
}

/* Implements an emulation of the unlink system call for regular files
   on the filesystem of size fssize pointed to by fsptr.

   This function is called only for the deletion of regular files.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 unlink.

*/
int __myfs_unlink_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        myfs_initialize(fsptr, fssize);
    }

    // find the parent directory of the file to delete
    myfs_node_t *parent = myfs_find_parent_node(fsptr, handle, path);
    if (!parent) {
        *errnoptr = ENOENT; // no such file or directory
        return -1;
    }

    // ensure the parent is a directory
    if (parent->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // not a directory
        return -1;
    }

    // get the basename of the file to delete
    const char *filename = myfs_get_basename(path);
    if (!filename || strlen(filename) > MYFS_MAX_NAME_LEN) {
        *errnoptr = ENAMETOOLONG; // name too long
        return -1;
    }

    // traverse the children of the parent directory to find the file
    size_t *child_offset_ptr = &parent->child_offset;
    while (*child_offset_ptr) {
        // get the current child node
        myfs_node_t *child = (myfs_node_t *)TO_PTR(*child_offset_ptr, fsptr);

        // check if the child's name matches the target filename
        if (strcmp(child->name, filename) == 0) {
            // ensure the child is a regular file
            if (child->type != MYFS_FILE) {
                *errnoptr = EISDIR; // is a directory
                return -1;
            }

            // unlink the file by updating the parent's child list
            *child_offset_ptr = child->next_sibling_offset;

            // optionally clear the file data for security purposes
            memset(child, 0, sizeof(myfs_node_t));

            return 0; // success
        }

        // move to the next sibling in the list
        child_offset_ptr = &child->next_sibling_offset;
    }

    *errnoptr = ENOENT; // file not found
    return -1;
}

/* Implements an emulation of the rmdir system call on the filesystem
   of size fssize pointed to by fsptr.

   The call deletes the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The function call must fail when the directory indicated by path is
   not empty (if there are files or subdirectories other than . and ..).

   The error codes are documented in man 2 rmdir.

*/
int __myfs_rmdir_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path) {
    // ensure the filesystem is initialized
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EINVAL; // invalid filesystem state
        return -1;
    }

    // root directory cannot be removed
    if (strcmp(path, "/") == 0) {
        *errnoptr = EBUSY; // operation not permitted for root
        return -1;
    }

    // find the parent node of the directory to be removed
    myfs_node_t *parent_node = myfs_find_parent_node(fsptr, handle, path);
    if (!parent_node) {
        *errnoptr = ENOENT; // parent directory not found
        return -1;
    }

    // extract the basename of the directory to be removed
    const char *basename = myfs_get_basename(path);
    if (!basename) {
        *errnoptr = ENOENT; // invalid path
        return -1;
    }

    // traverse the parent's child list to find the target directory node
    size_t *child_offset_ptr = &parent_node->child_offset;
    myfs_node_t *target_node = NULL;

    while (*child_offset_ptr) {
        myfs_node_t *current_node = (myfs_node_t *)TO_PTR(*child_offset_ptr, fsptr);

        // check if the current child matches the target directory
        if (strcmp(current_node->name, basename) == 0) {
            target_node = current_node; // directory found
            break;
        }

        // move to the next sibling
        child_offset_ptr = &current_node->next_sibling_offset;
    }

    // if the target directory node was not found, return an error
    if (!target_node) {
        *errnoptr = ENOENT; // directory does not exist
        return -1;
    }

    // ensure the target node is a directory
    if (target_node->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // target is not a directory
        return -1;
    }

    // ensure the directory is empty before removal
    if (myfs_count_children(fsptr, target_node) > 0) {
        *errnoptr = ENOTEMPTY; // directory is not empty
        return -1;
    }

    // unlink the directory from the parent's child list
    *child_offset_ptr = target_node->next_sibling_offset;

    // clear the target node's data to "delete" it
    memset(target_node, 0, sizeof(myfs_node_t));

    // update the parent's access time
    myfs_update_atime(parent_node);

    return 0; // successful removal
}

/* Implements an emulation of the mkdir system call on the filesystem
   of size fssize pointed to by fsptr.

   The call creates the directory indicated by path.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 mkdir.

*/
int __myfs_mkdir_implem(void *fsptr, size_t fssize, int *errnoptr, const char *path) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EINVAL; // invalid filesystem state
        return -1;
    }

    // root directory cannot be created again
    if (strcmp(path, "/") == 0) {
        *errnoptr = EEXIST; // root directory already exists
        return -1;
    }

    // locate the parent node of the directory to be created
    myfs_node_t *parent_node = myfs_find_parent_node(fsptr, handle, path);
    if (!parent_node) {
        *errnoptr = ENOENT; // parent directory does not exist
        return -1;
    }

    // ensure the parent node is a directory
    if (parent_node->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // specified parent is not a directory
        return -1;
    }

    // extract the name of the new directory
    const char *dirname = myfs_get_basename(path);
    if (!dirname || strlen(dirname) > MYFS_MAX_NAME_LEN) {
        *errnoptr = ENAMETOOLONG; // directory name is too long
        return -1;
    }

    // check if a file or directory with the same name already exists
    size_t child_offset = parent_node->child_offset;
    while (child_offset) {
        myfs_node_t *child = (myfs_node_t *)TO_PTR(child_offset, fsptr);
        if (strcmp(child->name, dirname) == 0) {
            *errnoptr = EEXIST; // directory or file already exists
            return -1;
        }
        child_offset = child->next_sibling_offset;
    }

    // allocate memory for the new directory node
    myfs_node_t *new_dir = (myfs_node_t *)myfs_alloc(handle, fsptr, sizeof(myfs_node_t));
    if (!new_dir) {
        *errnoptr = ENOSPC; // insufficient space for new directory
        return -1;
    }

    // initialize the new directory node
    memset(new_dir, 0, sizeof(myfs_node_t)); // zero out memory
    new_dir->type = MYFS_DIR; // set the node type as a directory
    strncpy(new_dir->name, dirname, MYFS_MAX_NAME_LEN); // copy the directory name
    new_dir->name[MYFS_MAX_NAME_LEN] = '\0'; // ensure null termination
    new_dir->parent_offset = TO_OFFSET(parent_node, fsptr); // set parent node offset
    clock_gettime(CLOCK_REALTIME, &new_dir->atim); // set access time
    clock_gettime(CLOCK_REALTIME, &new_dir->mtim); // set modification time

    // add the new directory as a child of the parent node
    myfs_add_child(fsptr, parent_node, new_dir);

    // update the modification time of the parent directory
    myfs_update_mtime(parent_node);

    return 0; // directory creation successful
}

/* Implements an emulation of the rename system call on the filesystem
   of size fssize pointed to by fsptr.

   The call moves the file or directory indicated by from to to.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   Caution: the function does more than what is hinted to by its name.
   In cases the from and to paths differ, the file is moved out of
   the from path and added to the to path.

   The error codes are documented in man 2 rename.

*/
int __myfs_rename_implem(void *fsptr, size_t fssize, int *errnoptr,
                         const char *from, const char *to) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized by checking the magic number
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EINVAL; // invalid argument if the filesystem is uninitialized
        return -1;
    }

    // locate the 'from' path and verify its existence
    myfs_node_t *from_node = myfs_find_node(fsptr, handle, from);
    if (!from_node) {
        *errnoptr = ENOENT; // no such file or directory
        return -1;
    }

    // ensure 'to' is not the root directory
    if (strcmp(to, "/") == 0) {
        *errnoptr = EBUSY; // cannot rename to the root directory
        return -1;
    }

    // check if the 'to' path already exists
    myfs_node_t *to_node = myfs_find_node(fsptr, handle, to);
    if (to_node) {
        // if 'to' is a directory, ensure it is empty before overwriting
        if (to_node->type == MYFS_DIR && myfs_count_children(fsptr, to_node) > 0) {
            *errnoptr = ENOTEMPTY; // directory not empty
            return -1;
        }

        // remove the existing 'to' node from its parent
        myfs_node_t *to_parent = myfs_find_parent_node(fsptr, handle, to);
        if (!to_parent) {
            *errnoptr = ENOENT; // parent directory of 'to' not found
            return -1;
        }

        // traverse the child list to unlink 'to_node'
        size_t *sibling_offset_ptr = &to_parent->child_offset;
        while (*sibling_offset_ptr) {
            myfs_node_t *current_node = (myfs_node_t *)TO_PTR(*sibling_offset_ptr, fsptr);
            if (current_node == to_node) {
                *sibling_offset_ptr = current_node->next_sibling_offset;
                memset(current_node, 0, sizeof(myfs_node_t)); // clear the 'to' node memory
                break;
            }
            sibling_offset_ptr = &current_node->next_sibling_offset;
        }
    }

    // find the parent of the 'from' node
    myfs_node_t *from_parent = myfs_find_parent_node(fsptr, handle, from);
    if (!from_parent) {
        *errnoptr = ENOENT; // parent directory of 'from' not found
        return -1;
    }

    // unlink 'from_node' from its parent's child list
    size_t *child_offset_ptr = &from_parent->child_offset;
    while (*child_offset_ptr) {
        myfs_node_t *current_node = (myfs_node_t *)TO_PTR(*child_offset_ptr, fsptr);
        if (current_node == from_node) {
            *child_offset_ptr = current_node->next_sibling_offset;
            break;
        }
        child_offset_ptr = &current_node->next_sibling_offset;
    }

    // find the parent of the 'to' path
    myfs_node_t *to_parent = myfs_find_parent_node(fsptr, handle, to);
    if (!to_parent || to_parent->type != MYFS_DIR) {
        *errnoptr = ENOTDIR; // 'to' parent is not a directory
        return -1;
    }

    // update the name of 'from_node' and reassign it as a child of 'to_parent'
    strncpy(from_node->name, myfs_get_basename(to), MYFS_MAX_NAME_LEN);
    myfs_add_child(fsptr, to_parent, from_node);

    // update access and modification times for involved directories
    myfs_update_atime(from_parent);
    myfs_update_mtime(to_parent);

    return 0; // success
}

/* Implements an emulation of the truncate system call on the filesystem
   of size fssize pointed to by fsptr.

   The call changes the size of the file indicated by path to offset
   bytes.

   When the file becomes smaller due to the call, the extending bytes are
   removed. When it becomes larger, zeros are appended.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 truncate.

*/
int __myfs_truncate_implem(void *fsptr, size_t fssize, int *errnoptr,
                           const char *path, off_t offset) {
    // cast the filesystem pointer to the filesystem handle
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized by checking the magic number
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EINVAL; // invalid argument if the filesystem is uninitialized
        return -1;
    }

    // locate the node corresponding to the given path
    myfs_node_t *file_node = myfs_find_node(fsptr, handle, path);
    if (!file_node) {
        *errnoptr = ENOENT; // error: no such file or directory
        return -1;
    }

    // ensure the path points to a regular file
    if (file_node->type != MYFS_FILE) {
        *errnoptr = EISDIR; // error: path points to a directory
        return -1;
    }

    // ensure the offset is not negative
    if (offset < 0) {
        *errnoptr = EINVAL; // error: invalid argument for negative offset
        return -1;
    }

    // case 1: if the file size needs to be reduced
    if ((size_t)offset < file_node->size) {
        // simply update the file size to the new smaller size
        file_node->size = offset;
    }
    // case 2: if the file size needs to be increased
    else if ((size_t)offset > file_node->size) {
        size_t current_size = file_node->size; // current file size
        size_t additional_size = offset - current_size; // extra space required

        // allocate the additional space
        void *new_data = myfs_alloc(handle, fsptr, additional_size);
        if (!new_data) {
            *errnoptr = ENOSPC; // error: not enough space
            return -1;
        }

        // initialize the newly allocated space with zeros
        memset(new_data, 0, additional_size);

        // if the file has no data yet, set the data offset
        if (file_node->size == 0) {
            file_node->data_offset = TO_OFFSET(new_data, fsptr);
        }

        // update the file size to the new larger size
        file_node->size = offset;
    }

    // update the modification time for the file
    clock_gettime(CLOCK_REALTIME, &file_node->mtim);

    return 0; // success
}

/* Implements an emulation of the open system call on the filesystem
   of size fssize pointed to by fsptr, without actually performing the opening
   of the file (no file descriptor is returned).

   The call just checks if the file (or directory) indicated by path
   can be accessed, i.e. if the path can be followed to an existing
   object for which the access rights are granted.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The two only interesting error codes are

   * EFAULT: the filesystem is in a bad state, we can't do anything

   * ENOENT: the file that we are supposed to open doesn't exist (or a
             subpath).

   It is possible to restrict ourselves to only these two error
   conditions. It is also possible to implement more detailed error
   condition answers.

   The error codes are documented in man 2 open.

*/
int __myfs_open_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path) {
    // cast the memory pointer to the filesystem handle
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // check if the filesystem is initialized by verifying the magic number
    if (!handle || handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EFAULT; // set error: filesystem is in a bad state
        return -1;
    }

    // locate the node corresponding to the specified path
    myfs_node_t *node = myfs_find_node(fsptr, handle, path);

    // if the node does not exist, return an error
    if (!node) {
        *errnoptr = ENOENT; // set error: no such file or directory
        return -1;
    }

    // successfully found the node; no further checks are needed as
    // this function only verifies the existence and accessibility
    return 0;
}

/* Implements an emulation of the read system call on the filesystem
   of size fssize pointed to by fsptr.

   The call copies up to size bytes from the file indicated by
   path into the buffer, starting to read at offset. See the man page
   for read for the details when offset is beyond the end of the file etc.

   On success, the appropriate number of bytes read into the buffer is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 read.

*/
int __myfs_read_implem(void *fsptr, size_t fssize, int *errnoptr,
                       const char *path, char *buf, size_t size, off_t offset)
{
    // cast the memory pointer to the filesystem handle
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is properly initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EFAULT; // set error: filesystem is in a bad state
        return -1;
    }

    // locate the file node corresponding to the given path
    myfs_node_t *file_node = myfs_find_node(fsptr, handle, path);
    if (!file_node) {
        *errnoptr = ENOENT; // set error: file not found
        return -1;
    }

    // ensure the path refers to a file (not a directory)
    if (file_node->type != MYFS_FILE) {
        *errnoptr = EISDIR; // set error: path is a directory
        return -1;
    }

    // if the offset is beyond the current file size, return EOF (0 bytes read)
    if ((size_t)offset >= file_node->size) {
        return 0;
    }

    // calculate the number of bytes available to read
    size_t bytes_available = file_node->size - offset;

    // determine the actual number of bytes to read
    size_t bytes_to_read = (size < bytes_available) ? size : bytes_available;

    // ensure the file has a valid data offset
    if (file_node->data_offset == 0) {
        *errnoptr = EIO; // set error: invalid data offset
        return -1;
    }

    // get a pointer to the file's data region in memory
    char *file_data = (char *)TO_PTR(file_node->data_offset, fsptr);

    // copy the requested bytes from the file into the provided buffer
    memcpy(buf, file_data + offset, bytes_to_read);

    // update the file's access time
    myfs_update_atime(file_node);

    // return the number of bytes read
    return (int)bytes_to_read;
}

/* Implements an emulation of the write system call on the filesystem
   of size fssize pointed to by fsptr.

   The call copies up to size bytes to the file indicated by
   path into the buffer, starting to write at offset. See the man page
   for write for the details when offset is beyond the end of the file etc.

   On success, the appropriate number of bytes written into the file is
   returned. The value zero is returned on an end-of-file condition.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 write.

*/
int __myfs_write_implem(void *fsptr, size_t fssize, int *errnoptr,
                        const char *path, const char *buf, size_t size, off_t offset)
{
    // cast the memory pointer to the filesystem handle
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is properly initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EFAULT; // set error: bad state
        return -1;
    }

    // locate the file node corresponding to the given path
    myfs_node_t *file_node = myfs_find_node(fsptr, handle, path);
    if (!file_node) {
        *errnoptr = ENOENT; // set error: file not found
        return -1;
    }

    // ensure the path refers to a file (not a directory)
    if (file_node->type != MYFS_FILE) {
        *errnoptr = EISDIR; // set error: is a directory
        return -1;
    }

    // calculate the total size required after writing
    size_t required_size = offset + size;

    // check if the required size exceeds the total filesystem size
    if (required_size > fssize) {
        *errnoptr = ENOSPC; // set error: not enough space
        return -1;
    }

    // if the required size exceeds the current file size, allocate more space
    if (required_size > file_node->size) {
        size_t additional_space = required_size - file_node->size;

        // attempt to allocate additional space
        char *new_data = myfs_alloc(handle, fsptr, additional_space);
        if (!new_data) {
            *errnoptr = ENOSPC; // set error: not enough space for allocation
            return -1;
        }

        // if the file has no data allocated yet, set the data offset
        if (file_node->size == 0) {
            file_node->data_offset = TO_OFFSET(new_data, fsptr);
        }

        // if there is a gap between the current size and the offset, fill it with zeros
        if (offset > file_node->size) {
            memset((char *)TO_PTR(file_node->data_offset, fsptr) + file_node->size, 0,
                   offset - file_node->size);
        }

        // update the file's size to reflect the newly required size
        file_node->size = required_size;
    }

    // get a pointer to the file's data region in memory
    char *file_data = (char *)TO_PTR(file_node->data_offset, fsptr);

    // copy the input buffer into the file data starting at the given offset
    memcpy(file_data + offset, buf, size);

    // update the modification time of the file
    clock_gettime(CLOCK_REALTIME, &file_node->mtim);

    // return the number of bytes written
    return (int)size;
}

/* Implements an emulation of the utimensat system call on the filesystem
   of size fssize pointed to by fsptr.

   The call changes the access and modification times of the file
   or directory indicated by path to the values in ts.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 utimensat.

*/
int __myfs_utimens_implem(void *fsptr, size_t fssize, int *errnoptr,
                          const char *path, const struct timespec ts[2]) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        // if not initialized, attempt to initialize the filesystem
        myfs_initialize(fsptr, fssize);
    }

    // locate the node corresponding to the file or directory at the specified path
    myfs_node_t *node = myfs_find_node(fsptr, handle, path);
    if (!node) {
        // if the node does not exist, return an error with ENOENT
        *errnoptr = ENOENT; // no such file or directory
        return -1;
    }

    // update access and modification times
    if (ts != NULL) {
        // if timestamps are provided, use them to update the node's times
        node->atim = ts[0]; // set access time
        node->mtim = ts[1]; // set modification time
    } else {
        // if no timestamps are provided, use the current system time
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now); // get the current time
        node->atim = now; // set access time to current time
        node->mtim = now; // set modification time to current time
    }

    return 0; // success
}

/* Implements an emulation of the statfs system call on the filesystem
   of size fssize pointed to by fsptr.

   The call gets information of the filesystem usage and puts in
   into stbuf.

   On success, 0 is returned.

   On failure, -1 is returned and *errnoptr is set appropriately.

   The error codes are documented in man 2 statfs.

   Essentially, only the following fields of struct statvfs need to be
   supported:

   f_bsize   fill with what you call a block (typically 1024 bytes)
   f_blocks  fill with the total number of blocks in the filesystem
   f_bfree   fill with the free number of blocks in the filesystem
   f_bavail  fill with same value as f_bfree
   f_namemax fill with your maximum file/directory name, if your
             filesystem has such a maximum

*/
int __myfs_statfs_implem(void *fsptr, size_t fssize, int *errnoptr, struct statvfs *stbuf) {
    myfs_handle_t *handle = (myfs_handle_t *)fsptr;

    // ensure the filesystem is initialized by verifying the magic number
    if (handle->magic != MYFS_MAGIC_NUMBER) {
        *errnoptr = EINVAL; // invalid argument error if the filesystem is uninitialized
        return -1;
    }

    // clear the statvfs structure to prepare for filling
    memset(stbuf, 0, sizeof(struct statvfs));

    // set the block size, which is constant for this filesystem
    stbuf->f_bsize = MYFS_BLOCK_SIZE;

    // calculate the total number of blocks in the filesystem
    // this is determined by dividing the total filesystem size by the block size
    stbuf->f_blocks = fssize / MYFS_BLOCK_SIZE;

    // determine the used space in the filesystem from the free_offset
    size_t used_space = handle->free_offset;

    // calculate the free space available in the filesystem
    size_t free_space = fssize - used_space;

    // calculate the number of free blocks based on the free space
    stbuf->f_bfree = free_space / MYFS_BLOCK_SIZE;

    // set f_bavail to the same value as f_bfree
    // this indicates the number of free blocks available to unprivileged users
    stbuf->f_bavail = stbuf->f_bfree;

    // set the maximum name length for files and directories
    stbuf->f_namemax = MYFS_MAX_NAME_LEN;

    return 0; // success
}
