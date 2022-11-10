### Implementation Details
This project is a simple simulator of a file system. Similar to FFS, the file system uses a superblock and several inodeblocks to map the entire system.

### Known Issues:
1. seg fault can happen rarely, but functions normal after fresh format, might be corrupt disk
2. 

### Test Cases
``` ./simplefs mydisk.200 200```
example run:
``` 
opened emulated disk image mydisk.200 with 200 blocks
 simplefs> format
disk formatted.
 simplefs> mount
disk mounted.
 simplefs> debug
superblock:
    200 blocks
    21 inode blocks
    2688 inodes
 simplefs> create
created inode 1
 simplefs> copyin 1.txt 1
65536 bytes copied
copied file 1.txt to inode 1
 simplefs> debug
superblock:
    200 blocks
    21 inode blocks
    2688 inodes
inode 1:
    size: 65536
    number of blocks: 16
    direct blocks: 22 23 24 25 26
    indirect block: 27
    indirect data blocks:  28 29 30 31 32 33 34 35 36 37 38
 simplefs> copyout 1 temp.txt
65536 bytes copied
copied inode 1 to file temp.txt
 simplefs>
 ```