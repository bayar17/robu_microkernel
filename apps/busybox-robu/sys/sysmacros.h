#include <bits/sysmacros.h>

#define major(dev) __mlibc_dev_major(dev)
#define minor(dev) __mlibc_dev_minor(dev)
#define makedev(major_num, minor_num) __mlibc_dev_makedev(major_num, minor_num)
