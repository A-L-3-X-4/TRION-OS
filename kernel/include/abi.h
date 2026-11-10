#pragma once
#define USER_BASE         0x8000000000
#define USER_TOP          0x10000000000
#define USER_CODE         0x8000001000
#define USER_STACK_TOP    0x8000100000
#define USER_STACK_PAGES  4

#define SEL_KCODE   0x08
#define SEL_KDATA   0x10
#define SEL_UCODE32 0x18
#define SEL_UDATA   (0x20 | 3)
#define SEL_UCODE   (0x28 | 3)
#define SEL_TSS     0x30

#define SYS_EXIT      0
#define SYS_WRITE     1
#define SYS_YIELD     2
#define SYS_GETTID    3
#define SYS_SLEEP_MS  4
#define SYS_CLOCK_NS  5
#define SYS_GETPID    6
#define SYS_PORT_ALLOC   7
#define SYS_PORT_DESTROY 8
#define SYS_PORT_DEALLOC 9
#define SYS_PORT_INSERT  10
#define SYS_PORT_INFO    11
#define SYS_PORT_QLIMIT  12
#define SYS_MSG_SEND     13
#define SYS_MSG_RECV     14
#define SYS_CLOSE        15
#define SYS_DUP          16
#define SYS_EVENT_CREATE 17
#define SYS_EVENT_OP     18
#define SYS_WAIT         19
#define SYS_SEM_CREATE   20
#define SYS_SEM_RELEASE  21
#define SYS_NS_MKDIR       22
#define SYS_NS_INSERT      23
#define SYS_NS_OPEN        24
#define SYS_NS_UNLINK      25
#define SYS_NS_PUBLISH     26
#define SYS_NS_LOOKUP_PORT 27
#define SYS_NS_QUERY       28
#define SYS_OBJ_GETSEC     29
#define SYS_OBJ_SETSEC     30
#define SYS_TOKEN_INFO     31
#define SYS_FILE_OPEN      32
#define SYS_FILE_READ      33
#define SYS_FILE_WRITE     34
#define SYS_FILE_SEEK      35
#define SYS_FILE_STAT      36
#define SYS_FILE_SETSIZE   37
#define SYS_FILE_READDIR   38
#define SYS_FILE_FLUSH     39
#define SYS_FS_MKDIR       40
#define SYS_FS_REMOVE      41
#define SYS_FS_RENAME      42
#define SYS_FS_STAT        43
#define SYS_FS_STATFS      44

#define SYS_DEV_OPEN       45
#define SYS_DEV_READ       46
#define SYS_DEV_WRITE      47
#define SYS_DEV_IOCTL      48
#define SYS_DEV_SEEK       49
#define SYS_VM_ALLOC       50
#define SYS_VM_FREE        51
#define SYS_VM_PROTECT     52
#define SYS_VM_QUERY       53
#define SYS_TIME_REAL      54
#define SYS_COUNT          55

#define VM_PROT_R      0x1
#define VM_PROT_W      0x2
#define VM_PROT_X      0x4
#define VM_RESERVE     0x100
#define VM_COMMIT      0x200
#define VM_DECOMMIT    0x400
#define VM_RELEASE     0x800
#define VM_GRANULARITY 0x10000
#define VM_ST_FREE      0
#define VM_ST_RESERVED  1
#define VM_ST_COMMITTED 2
#define VM_TY_NONE      0
#define VM_TY_PRIVATE   1
#define VM_TY_IMAGE     2

#define SID_SYSTEM        1
#define SID_EVERYONE      2
#define SID_CREATOR_OWNER 3
#define SID_USER_BASE     1000

#define PRIV_BYPASS_ACL      0x1
#define PRIV_TAKE_OWNERSHIP  0x2

#define ACE_ALLOW         0
#define ACE_DENY          1
#define ACE_INHERIT       0x1
#define ACE_INHERIT_ONLY  0x2

#define SD_DACL_PRESENT   0x1
#define SEC_OWNER         0x1
#define SEC_DACL          0x2
#define SD_ACE_MAX        12
#define SEC_GROUPS_MAX    8

#define OBJ_DIR       1
#define OBJ_EVENT     2
#define OBJ_SEMAPHORE 3
#define OBJ_PORTREF   4
#define OBJ_VOLUME    5
#define OBJ_FILE      6
#define OBJ_DRIVER    7
#define OBJ_DEVICE    8
#define OBJ_DEVFILE   9

#define DEV_READ_DATA   0x0001
#define DEV_WRITE_DATA  0x0002
#define DEV_IOCTL       0x0004
#define DEV_VALID_HANDLE (DEV_READ_DATA | DEV_WRITE_DATA | DEV_IOCTL)
#define DEVCTL_FLUSH    1
#define DEVCTL_GETSIZE  0x10
#define DEVCTL_SECTOR   0x11
#define DEVCTL_USER     0x100
#define MEM_IOCTL_FAIL_NEXT  (DEVCTL_USER + 0x01)
#define MEM_IOCTL_COUNTERS   (DEVCTL_USER + 0x02)
#define SLOW_IOCTL_DELAY     (DEVCTL_USER + 0x11)
#define CNT_IOCTL_GET        (DEVCTL_USER + 0x21)
#define CNT_IOCTL_ROFS       (DEVCTL_USER + 0x22)
#define XOR_IOCTL_KEY        (DEVCTL_USER + 0x31)
#define DIR_QUERY     0x0001
#define DIR_TRAVERSE  0x0002
#define DIR_CREATE    0x0004
#define DIR_DELETE_CHILD 0x0008
#define PORTREF_CONNECT  0x0001

#define ACCESS_DELETE            0x00010000
#define ACCESS_READ_CONTROL      0x00020000
#define ACCESS_WRITE_DAC         0x00040000
#define ACCESS_WRITE_OWNER       0x00080000
#define ACCESS_SEC_RIGHTS        0x000E0000
#define ACCESS_MAXIMUM           0x02000000
#define ACCESS_GENERIC_ALL       0x10000000
#define ACCESS_GENERIC_EXECUTE   0x20000000
#define ACCESS_GENERIC_WRITE     0x40000000
#define ACCESS_GENERIC_READ      0x80000000
#define ACCESS_GENERIC_MASK      0xF0000000
#define ACCESS_SYNCHRONIZE       0x00100000
#define EVENT_MODIFY_STATE       0x00000002
#define SEMAPHORE_MODIFY_STATE   0x00000002
#define EVENT_SET    1
#define EVENT_RESET  2

#define FILE_READ_DATA     0x0001
#define FILE_WRITE_DATA    0x0002
#define FILE_CREATE        0x0004
#define FILE_DELETE_CHILD  0x0008
#define FILE_VALID_HANDLE  (FILE_READ_DATA | FILE_WRITE_DATA)

#define FD_OPEN          0
#define FD_CREATE        1
#define FD_OPEN_IF       2
#define FD_OVERWRITE_IF  3
#define FD_MASK          0xFF
#define FO_DIRECTORY     0x100

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define FT_FILE 1
#define FT_DIR  2
#define FA_RDONLY  0x01
#define FA_HIDDEN  0x02
#define FA_SYSTEM  0x04
#define FA_DIR     0x10
#define FA_ARCHIVE 0x20

#define E_ACCES    13
#define E_NOENT    2
#define E_XDEV     18
#define E_IO       5
#define E_BUSY     16
#define E_ISDIR    21
#define E_FBIG     27
#define E_ROFS     30
#define E_NAMETOOLONG 36
#define E_EXIST    17
#define E_NOTDIR   20
#define E_NOTEMPTY 39
#define E_PERM     1
#define E_BADF     9
#define E_AGAIN    11
#define E_NOMEM    12
#define E_FAULT    14
#define E_INVAL    22
#define E_NOSPC    28
#define E_PIPE     32
#define E_NOSYS    38
#define E_SRCH     3
#define E_2BIG     7
#define E_NOEXEC   8
#define E_NODEV    19
#define E_MFILE    24
#define E_NOTTY    25
#define E_SPIPE    29
#define E_RANGE    34
#define E_DEADLK   35
#define E_NOTSUP   95
#define E_MSGSIZE  90
#define E_TIMEDOUT 110
#define E_CANCELED 125

#define IPC_MSG_MAX      512
#define IPC_MAX_DESC     8
#define IPC_TIMEOUT_INF  0xFFFFFFFFFFFFFFFF

#define MSG_MOVE_RECEIVE    16
#define MSG_MOVE_SEND       17
#define MSG_MOVE_SEND_ONCE  18
#define MSG_COPY_SEND       19
#define MSG_MAKE_SEND       20
#define MSG_MAKE_SEND_ONCE  21
#define MSG_BITS(remote, local)  ((remote)|((local)<<8))

#define PT_RECV   1
#define PT_SEND   2
#define PT_SONCE  4
#define PT_DEAD   8

#ifndef __ASSEMBLER__
#include <stdint.h>
struct msg_hdr  { uint32_t bits, size, remote, local, id, ndesc; };
struct msg_desc { uint32_t name, disp; };
struct port_info { uint32_t type, urefs, qlen, qlimit; };
struct ns_dirent { char name[32]; uint32_t type; };
struct ace        { uint8_t type, flags; uint16_t rsvd; uint32_t sid, mask; };
struct secdesc    { uint32_t owner, flags, nace; struct ace aces[SD_ACE_MAX]; };
struct token      { uint32_t user, privs, ngroups, groups[SEC_GROUPS_MAX]; };
struct file_stat  { uint64_t size; uint32_t type, attr; uint64_t ctime, mtime, atime; };
struct fs_dirent  { char name[256]; uint32_t type, attr; uint64_t size; };
struct dev_ioctl  { uint64_t in, inlen, out, outlen; };
struct fs_statfs  { uint64_t total_bytes, free_bytes; uint32_t cluster_bytes, flags; char fs[8], label[12]; };
struct vm_info    { uint64_t base, size, abase; uint32_t prot, aprot, state, type; };
#endif

#define TASK_FAULT_BASE 100

#define UT_CPL        0x0000
#define UT_HELLO      0x0200
#define UT_REGS       0x0400
#define UT_BADSYS     0x0600
#define UT_FAULT_W    0x0800
#define UT_FAULT_K    0x0A00
#define UT_FAULT_PRIV 0x0C00
#define UT_ISO        0x0E00
#define UT_SPIN       0x1000
#define UT_FPU        0x1200
#define UT_PORT_BASIC 0x1400
#define UT_RPC_SERVER 0x1C00
#define UT_OBJ_BASIC  0x2400
#define UT_NS_BASIC   0x3400
#define UT_NS_SERVER  0x4400
#define UT_NS_CLIENT  0x4800
#define UT_SEC_ALICE  0x5400
#define UT_SEC_BOB    0x6400
#define UT_FS         0x7400
#define UT_DEV        0x8400
#define UT_OBJ_WAITER 0x2C00
#define UT_RPC_CLIENT 0x2000
