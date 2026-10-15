CSE321 - Operating Systems
Lab Project: SimpleFS - Implementation of a Simple File System in C
Summer 2026

Section: 07
Group: 14

Group Members:
1. Afia Tasnim        - ID: 23301678
2. Md Rakib Hasan      - ID: 23201141


COMPILATION COMMANDS
---------------------
gcc -Wall -Wextra -std=c11 simplefs_builder.c -o simplefs_builder
gcc -Wall -Wextra -std=c11 simplefs_adder.c -o simplefs_adder

Both programs compile cleanly with no warnings under -Wall -Wextra -std=c11.


EXECUTION EXAMPLES
--------------------
Create a new empty SimpleFS image:
    ./simplefs_builder --image disk.img

Add a file from the current working directory into the image:
    ./simplefs_adder --input disk.img --file test1.txt
    ./simplefs_adder --input disk.img --file test2.txt
    ./simplefs_adder --input disk.img --file test3.txt

Inspect the resulting image:
    xxd disk.img
    xxd -s 4096  -l 16 disk.img   (inode bitmap)
    xxd -s 8192  -l 16 disk.img   (data bitmap)
    xxd -s 16384 -l 128 disk.img  (root directory: "." and "..")


BRIEF DESCRIPTION OF THE IMPLEMENTATION
------------------------------------------
SimpleFS is a small educational file system stored as a flat 262144-byte
(256 KiB) binary image made up of 64 fixed 4096-byte blocks:

    Block 0       - Superblock (magic number, block size, block counts,
                     region locations, root inode number)
    Block 1       - Inode bitmap (1 bit per inode, 32 inodes total;
                     0 = free, 1 = allocated)
    Block 2       - Data bitmap (1 bit per data block, 60 data blocks total)
    Block 3       - Inode table (32 inodes x 128 bytes each)
    Blocks 4-63   - Data region (60 blocks of raw file/directory data)

simplefs_builder.c formats a brand-new image: it zero-fills all 64 blocks,
writes the superblock, marks inode 1 and data block 4 as allocated in their
respective bitmaps, initializes the root inode (type = directory, links = 2,
size = 128 bytes, direct[0] = 4), and writes the "." and ".." directory
entries (each pointing back to inode 1, since the root has no parent) into
data block 4.

simplefs_adder.c adds one regular file from the current working directory
into an existing image:
  1. Opens the image and validates the superblock magic number.
  2. Opens the source file and rejects it if it is missing, larger than
     12288 bytes (3 direct blocks x 4096 bytes), or if its name is longer
     than 58 characters.
  3. Computes the number of required blocks with
     (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE (0 for an empty file).
  4. Rejects duplicate file names already present in the root directory.
  5. Uses first-fit scans over the inode bitmap and data bitmap to find a
     free inode and enough free data blocks, tracking the newly allocated
     absolute block numbers.
  6. Copies the source file's bytes into the allocated blocks using
     zero-filled 4096-byte buffers, so any unused tail of the final block
     stays zero.
  7. Writes the new file's inode (type = file, links = 1, actual size in
     bytes, direct[] pointing at the allocated blocks) into the inode table,
     updates both bitmaps on disk, writes a new directory entry (inode
     number, type, null-terminated name) into the first free slot in the
     root directory (entries 2-63), and increases the root inode's size by
     64 bytes (sizeof(dirent_t)) per new entry.

All allocation is strictly first-fit: both find_free_inode() and
find_free_data_block() scan from the beginning of their bitmap and return
the first free bit found, converting between bitmap index, inode number,
inode-table offset, and absolute block number as described in the
specification.


CONTRIBUTION OF EACH GROUP MEMBER
------------------------------------
Afia Tasnim (23301678):
    - Implemented and tested simplefs_builder.c (superblock, inode/data
      bitmap initialization, root inode, "." and ".." entries).
    - Verified image layout and initial state using hex inspection.

Md Rakib Hasan (23201141):
    - Implemented and tested simplefs_adder.c (first-fit inode/data-block
      allocation, duplicate/name/size validation, file-data copying,
      directory-entry creation, root inode size updates).
    - Ran and verified all specification test cases (empty image,
      superblock, bitmaps, root directory, one/two/three-block files,
      maximum file size, duplicate names, missing files, full directory).

we reviewed and understand the full implementation, including
block/bitmap-index/inode-number conversions and first-fit allocation, for
the individual viva.


KNOWN LIMITATIONS / PROBLEMS
--------------------------------
- As specified, SimpleFS only supports a single flat root directory (no
  subdirectories), no file deletion, renaming, links, permissions,
  journaling, checksums, mounting, or caching.
- Maximum of 31 regular files can be stored (32 inodes total, minus 1 for
  the root), and the maximum file size is 12288 bytes (3 direct blocks).
- Files must reside in the current working directory; no path handling is
  implemented (not required by the specification).
- No known bugs; all test cases described in the project specification
  (Sections 19.I - 19.XV) pass as expected.
